# 2026-09-16 DSH 导出终态：GLM 独立机械测试（测试矩阵与记录）

状态：本批 GLM 4 路径已完成。**中途状态更新**：Kimi 的生产头与接线在本批执行期间落地，
静态合同共四轮单次运行：run1（落地前）5 项接线失败=当时的待集成记录；run2（落地后）10/10；run3（主线程审查修订后）13/13；run4（删除 W5 残留全文件计数后）13/13。
C++ 测试现已可编译（主线程统一管理编译）；静态检查与 C++ 表通过都不是运行时/CPU 提交/GPU/游戏证据。
基线：branch `codex/v1.22-release-integration-20260914`，用户授权的快照备份 commit
`30c9163ea85081b367b114bb5e872362fb5dd065`（用户授权的未验收 WIP 备份）；工作树开工 274 dirty。
依据：`2026-09-16-recorder-export-terminal-contract.md` 与本批计划冻结接口
（`ExportProgress`/`ExportOutcome`/`ClassifyExportProgress`，namespace
`dxvk::war3::tools::history`，`MaxSlots` 沿用 `war3_frame_history_core.h`）。

## 1. 交付物身份

| 文件 | size (bytes) | SHA-256 |
| --- | --- | --- |
| `AutoTest/test_frame_history_export_core.cpp` | 7107 | `ff44e794443eac8d08801d0c59947a5ff8d426a73eb8f22d74c161b5eff2d9ef` |
| `AutoTest/test_frame_history_export_contract_static.py` | 9103 | `5dbec77bd6e127a9d73d9eaeed1eb8d633c6a5ef6cf9d961b3897efca216bb40` |
| `docs/agent-history/2026-09-16-dsh-export-tests.md`（本文件） | 见文件系统 | 按惯例不自哈希 |
| `docs/agent-history/2026-09-16-dsh-mechanical-batch-01.md`（仅两处事实更正） | 8691 | `40fd300c557ec1ae23e9b4a3e50ecc7384dfd40a270544d81095a96679f5afb6` |

更正范围仅限上一份报告两处：stderr 事实（15/15 脚本 stderr 非空，unittest 摘要输出到
stderr，实测 108–151 bytes；stdout 全 0；与父线程
`dsh_mechanical_20260916_parent_review/run-all-r2.json` 一致）与 py_compile/ast.parse
的执行者区分（py_compile 由主线程复验；我方仅为 ast.parse）。未改任何旧 receipt/证据。

## 2. C++ 表驱动测试矩阵（test_frame_history_export_core.cpp，21 行 + 稳定性二遍）

直接 include 冻结路径生产头 `../src/d3d9/war3/tools/war3_frame_history_export_core.h`；
`MaxSlots` 引用 `../src/d3d9/war3/tools/war3_frame_history_core.h` 的同一常量（无本地副本）。
编译期断言：`ClassifyExportProgress` 保持 `noexcept`（冻结签名）、`ExportProgress`
保持 trivially copyable（纯 CPU 无所有权值类型）。二遍重复调用必须逐行一致（无内部状态）。

| # | 行名 | 输入 (retained,queued,done,failed,valid) | 期望 |
| --- | --- | --- | --- |
| 1 | requests-invalid-flag | (272,272,272,0,false) | InvalidProgress（request 缺失） |
| 2 | retained-zero | (0,0,0,0,true) | InvalidProgress（retained=0） |
| 3 | retained-maxslots-plus-one | (MaxSlots+1,×3,0,true) | InvalidProgress（retained>MaxSlots） |
| 4 | retained-uint32-max | (UINT32_MAX,×3,0,true) | InvalidProgress |
| 5 | failed-greater-than-done | (4,4,2,3,true) | InvalidProgress（failed>done） |
| 6 | done-greater-than-queued | (4,2,3,0,true) | InvalidProgress（done>queued） |
| 7 | queued-greater-than-retained | (4,5,3,0,true) | InvalidProgress（queued>retained） |
| 8 | done-uint32-max-overflow-bait | (MaxSlots,MaxSlots,UINT32_MAX,1,true) | InvalidProgress（禁止溢出加总） |
| 9 | all-contradictions-at-once | (UINT32_MAX,×4,false) | InvalidProgress |
| 10 | pending-none-done | (4,4,0,0,true) | Pending |
| 11 | pending-partial-failure-waits | (4,4,2,1,true) | Pending（部分失败仍待全部结算） |
| 12 | pending-partially-queued | (4,2,1,0,true) | Pending（部分待排队） |
| 13 | pending-maxslots-one-remaining | (MaxSlots,MaxSlots,MaxSlots-1,3,true) | Pending |
| 14 | pending-done-queued-below-retained | (4,3,3,1,true) | Pending（done==queued 边界） |
| 15 | complete-all-done-no-failures | (4,4,4,0,true) | Complete |
| 16 | complete-maxslots-exact-boundary | (MaxSlots,MaxSlots,MaxSlots,0,true) | Complete |
| 17 | complete-single-slot | (1,1,1,0,true) | Complete |
| 18 | readback-failed-partial | (4,4,4,2,true) | ReadbackFailed |
| 19 | readback-failed-all-failed | (4,4,4,4,true) | ReadbackFailed |
| 20 | readback-failed-maxslots-exact | (MaxSlots,MaxSlots,MaxSlots,1,true) | ReadbackFailed |
| 21 | readback-failed-failed-eq-done-max | (MaxSlots,MaxSlots,MaxSlots,MaxSlots,true) | ReadbackFailed（failed==done 等值边界） |

覆盖对照计划要求：四种结论 ✓（10-14 Pending、15-17 Complete、18-21 ReadbackFailed、1-9
InvalidProgress）；0/MaxSlots/MaxSlots+1 ✓（2/16/20/21/3）；UINT32_MAX ✓（4/8/9）；
各字段逆序矛盾 ✓（5/6/7 及 3/4）；request 缺失 ✓（1）；部分失败与待排队 ✓（11/12/14）；
边界稳定性 ✓（二遍一致 + noexcept/trivially-copyable 编译期断言）。

## 3. 生产接线静态检查矩阵（test_frame_history_export_contract_static.py，13 项=5 接线+5 保留+3 反例自测）

函数体限定（按主线程审查修订）：签名用空白弹性正则定位，函数体用字面量/注释感知的
括号配对提取（mask 长度保持），不做全文件字符串计数。接线断言一律运行在注释与字面量
双双屏蔽后的函数体上——注释或字符串里的入口名不算真实调用；保留族中除 P1（其载荷本身
是 wire 字面量，采用"屏蔽注释、保留字面量"视图）外同样使用屏蔽体。SCOPE 常量明确标注
仅静态文本。StaticCheckSelfTests 用真实 mask/extract/has_call 助手在合成文本上证明：
注释伪调用、字符串伪调用、缺失定义体均无法通过接线检查。

接线族（ExportWiringTargetState，针对冻结目标态，落地前预期失败=待集成）：
| # | 检查 | run1 结果 |
| --- | --- | --- |
| W1 | status 函数体内调用 `settleExportProgressLocked(` | run1 FAIL（落地前）；run2 PASS |
| W2 | nextExport 函数体内调用 `settleExportProgressLocked(` | run1 FAIL（落地前）；run2 PASS |
| W3 | status 内旧重复终态写 `state=failed?Fault:Complete` 与条件 `done==orderCount&&queued==orderCount` 消失 | run1 FAIL；run2 PASS（两模式均已消失） |
| W4 | nextExport 内旧内联终态（`image-export-failed`、直接写 `m->state=Impl::Complete`、`hud().state=Impl::Complete`）消失 | run1 FAIL；run2 PASS（`image-export-failed` 仅存在于头的 DecideExportCommit） |
| W5 | `test_settle_entry_definition_and_real_calls_in_caller_bodies`：提取 settle 定义函数体 + status/nextExport 屏蔽体内真实调用（主线程复审后已删除残留的全文件计数断言，现仅函数体判定） | run1/2 为旧口径；run3 PASS；run4（删计数后）PASS |

保留族（ExportContractPreserved，落地前后都必须成立）：
| # | 检查 | run1 结果 |
| --- | --- | --- |
| P1 | status 体保留 `{"schema",2}`、`{"state",uint32_t(state.load())}`、`{"captureComplete",false}`、`{"rootCauseReady",false}` | PASS |
| P2 | core 头 `CaptureLifecycle` 状态编号 Idle=0…Discard=8 原样 | PASS |
| P3 | nextExport 体保留 `std::try_to_lock`、`owns_lock` 早退、`m->cancelled.load()`、`state!=Exporting` 早退（不复活 Fault/Discard） | PASS |
| P4 | `fail` 体保留 `state=Fault;fault=reason;recording=false;` | PASS |
| P5 | status/nextExport/**settle 定义体/共享生产 header** 均不触碰 `packageReady`（屏蔽注释后判定；图像完成不签发包就绪） | run1/2 为旧口径；run3（扩展范围）PASS |

run1 汇总（落地前）：`Ran 10 tests; FAILED (failures=5)`，exit 1——当时的待集成精确记录；
run2 汇总（落地后，旧口径 10 项）：`Ran 10 tests ... OK`，exit 0。
run3 汇总（主线程审查修订后，13 项）：`Ran 13 tests ... OK`，exit 0——接线断言全部改用
屏蔽后函数体（W5 重写为定义体提取+调用者体内真实调用），packageReady 检查扩展到 settle
体与共享 header，3 个反例自测证明注释/字符串伪调用与缺失定义体无法通过。
run4 汇总（主线程复审后，13 项）：删除 W5 中残留的全文件计数断言后 `Ran 13 tests ... OK`，
exit 0——现仅剩函数体限定判定（定义体提取 + 两个调用者屏蔽体内真实调用 + 3 个反例自测）。
run1/run2/run3 原始输出原样保留未覆盖。

## 4. 实际命令（cwd=集成树根；`python`=3.13.11，均 `-B`）

1. `python -B -c "ast.parse(...)"` 仅适用于 Python 新文件（static 合同测试）→ 通过；
   C++ 测试只能做纯文本/括号检查（字符串/注释感知括号平衡 0/0、21 行用例名齐全、
   无 here-string 残留、无双反斜杠），不称 ast.parse、不编译。
2. `python -B AutoTest/test_frame_history_export_contract_static.py -v` → run1（落地前）单次运行，
   `Ran 10 tests ... FAILED (failures=5)`，exit 1；输出存
   run1/run2 输出存 `static_contract_run1/static_contract_run2.{stdout,stderr}.txt`（原样保留）。
3. run3（主线程审查修订后新增运行）：`Ran 13 tests ... OK`，exit 0，输出存
   `static_contract_run3.{stdout,stderr}.txt`。
4. run4（删除 W5 残留全文件计数断言后唯一新增运行）：`Ran 13 tests ... OK`，exit 0，输出存
   `static_contract_run4.{stdout,stderr}.txt`；run1/run2/run3 均原样保留，互不覆盖。
3. 未运行编译器/Ninja/native gate/游戏/GPU；C++ 测试未编译（授权归主线程）。
5. 运行纪律：run1→run2 仅因头/接线落地（失败状态已改变）各一次；run3 仅因主线程审查
   修订（检查口径变更）一次；run4 仅因删除 W5 残留全文件计数一次。未反复执行同状态已知
   失败测试。

## 5. 待集成与限制

- 已落地（本批执行期间，Kimi）：`war3_frame_history_export_core.h`——只读核对与冻结接口逐字段一致（namespace/结构/枚举顺序/noexcept/MaxSlots 复用），并含授权内的 SampleExportProgress/ExportCommitDecision/ShouldCommitExport 最小提交边界；
  `Impl::settleExportProgressLocked` 接线亦已落地（run2/run3 全绿）：status 与 nextExport
  均已改为经唯一入口提交终态，两函数体中的旧重复终态分支已消失。
- 状态分列（截至本批最终只读巡检，以后续巡检为准）：
  - 主线程计划内（GLM 4 路径）：3 个新文件完成 + 上一份报告两处授权更正完成；
    C++ 表驱动测试待主线程编译运行（头已落地，现可编译）。
  - Kimi 新文件：`AutoTest/test_frame_history_export_commit.cpp` 已出现（17,058 bytes），
    其验收归 Kimi/主线程，不在我的范围；`docs/agent-history/2026-09-16-dsh-export-architecture.md` 已出现。
  - Kimi 共享提交应用搬入同一生产 header：已落地（主线程确认，含 242 项真实 shared
    commit Win32 编译运行检查）。本文件此前"尚未落地"的表述对应的是更早时间点的只读巡检
    快照，非当前状态；run4 已在落地后的当前文本上通过，我方不再轮询其进展。
- 静态检查不冒充运行时/CPU 提交/GPU/游戏证据；它只证明文本合同。C++ 表通过也只证明
  `ClassifyExportProgress` 的纯函数语义，不证明接线调用、提交副作用或 HUD 一致性。
- 接线检查的具体形态（哪些模式算"旧分支消失"）基于当前 `war3_frame_history.cpp` 的真实文本；
  若 Kimi 的实现以语义等价但文本不同的方式重写，可能出现需要主线程裁定的假阳性，届时以
  失败消息中的函数体原文判定，不由我放宽断言。
- 本批未修改 Kimi 的任何文件、原测试/阈值/白名单、总日志、Git；旧证据未删改；
  上一份报告仅做两处授权的事实更正。

> 修订记录：①身份更正——初稿曾写入未计算的占位 SHA-256，交付前已用实际重算值替换（上方为最终值），并修正基线分支名笔误；本文件自身不自哈希。
> ②主线程审查修订（第二批）——W5 由全文件计数改为定义体提取+调用者屏蔽体内真实调用，
> 新增 3 个反例自测；packageReady 检查扩展到 settle 定义体与共享 header；更正 C++ 文件
> 只能文本/括号检查的表述；去除已落地后仍称各自判断终态的过期矛盾描述；新增 run3（当时唯一
> 新增运行），run1/run2 原证据未覆盖。
> ③主线程复审（同批）——删除 W5 残留的全文件计数断言（masked_occurrences>=3），仅保留
> 定义体提取与两个调用者屏蔽体检查；新增 run4 唯一运行；注明 Kimi 共享提交已落地、
> 此前巡检为旧时间点快照。