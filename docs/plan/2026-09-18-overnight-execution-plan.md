> ⚠️ **历史文档 —— 已过期（2026-09-18 round 79 标注）**
>
> 本文描述的是**当时**的现场与计划。**当前权威事实**见 `2026-09-18-objective-evidence-ledger.md`：
>
> ```
> 站点 E:\Work\Warcraft III\d3d9.dll = 36,288,789 B / F275545BAA65A015…5CF07FF3 = 基线 ⇒ **未部署**
> 写方 = **恒定 paletteObject version=4**（已删除 firstSightUsed() 动态选版本）
> 实机采集 = **未获授权、零采集**（协议见 2026-09-18-stageE-live-capture-protocol-v4.md）
> 门禁 = 静态 263/0（**不含**门禁外 40 个文件里的 3 个既有红）
> 五项裁定待决（窗口维度导出层 / 双链路由 / 实机授权 / 构建可复现 / 门禁外三红与覆盖缺口）
> ```
>
> **若本文与上述冲突，以上述为准。** 尤其是：本文若说"现场 DLL 已部署/勿动"，
> 那是**当时**的状态；**今天站点是基线、未部署**。
>
> ---
>
# 2026-09-18 夜间持续执行计划（主线程亲执行版）

> 本文件是**防上下文丢失**的执行计划：任何一棒接手都必须先读它 + 交接文档 `2026-09-18-deepseek-overnight-handoff.md`。
> 用户 2026-09-18 的明确指令：**主线程必须亲自做最重的任务并持续推进**，子代理只是辅助；需要等待时用**显式阻塞等待**，不得靠 goal 轮询空转；**不得给自己设轮次上限**；持续到 **07:30**。

## 0. 角色纪律（硬性）
1. **每一轮主线程至少亲自完成一件最重的工作**（读代码/改代码/构建/跑门禁/驱动实机/写证据文档），不允许"只派子代理然后空等"。
2. 子代理只用于**可并行且自包含**的辅助件（长跑差分、文档生成、只读勘察），必须有**明确的验收物**与**显式等待**。
3. 等待用**阻塞式**：`job_output(wait: true, timeout_ms)`、`list_agents`+必要时 `send_message`、或 `sleep`——**不要**用 goal 轮次来"检查进度"。
4. **不设轮次上限**；目标只有到 07:30 才收尾（或用户改变指令）。
5. 每完成一件，**同 checkpoint** 追加 `docs/plan/2026-09-18-overnight-progress-log.md`（真实时间戳）+ 更新 `docs/agent-history/DEVELOPMENT_CHANGELOG.md`。

## 1. 不可变事实（防丢失，实测值）
- 唯一写入树：`E:\Mycode\Source\Repos\War3MapReforge\Core\Base\Graphics\dxvk-v1.22-integration-20260914`（B 树，未提交）。
- **实机启动目录 = `E:\Work\Warcraft III`**（YDWE 从这里启动；`E:\Work\War3` 只是副本，别用）。
- **现场 DLL（禁止替换）= `E:\Work\Warcraft III\d3d9.dll`** = 36,271,456 B / `A0A51AF2BB9091B2C677DC34BEBDF76C02347049A89D0BB2AEF4C37518E52134`；备份：`E:\Work\War3-test-build-20260917\rollback\d3d9.dll.A0A51AF2_backup_20260918`（+`.sha256`）。
- 仓库 DLL（可重建）= 36,270,708 B / `EB9D51DE025CE8173400CE9C860FA70AB81D1E106B594DD3BF853944C9426A11`。
- 门禁基线（不得回归）：`ninja -n` no work、meson **83/83**、全量静态 **249/249**、M1 差分 **3,098,194/0**、M2 链差分 **10,357,597/0**、记录器 **23/23**、成本 **PASS(38/38)**、生命周期 **187/187**、解析器静态 94/94、根读方 55/55、往返 **CERTIFIED(A–F)**。
- 取证启动器（已核验内容）：`E:\Work\War3-test-build-20260917\启动-取证-含对象级子门.cmd`（`DXVK_WAR3_FRAME_EVIDENCE=1` + `DXVK_WAR3_FRAME_EVIDENCE_PALETTE_OBJECT=1`，`cd /d "E:\Work\Warcraft III"`，经 `bin\YDWEConfig.exe -launchwar3`）。
- 导出落点：`E:\Work\Warcraft III\WarVK\Log\FrameEvidence\cpu-*.json`；判定器：`py AutoTest\analyze_palette_object_evidence.py <json> --output <out.json>`（B 树根目录）。
- 控制面（生产代码）：`src/d3d9/war3/tools/war3_frame_evidence.cpp` 的 `control()`，动作 `arm`/`status`/`trigger(postPresents)`/`freeze`/`export`/`discard`。

## 2. 任务队列（按优先级，主线程逐项亲办）
- **T1（最高，主线程亲办）**：实机对象级取证 ×2（高压中 / 压力解除后）。成功判据＝**新** `cpu-*.json` 头块 `paletteObject` 非全 0。
  已知两次自动化尝试都"游戏起得来、导出产不出"⇒ **主线程必须自己把控制面找出来并触发**（读 `control()` 的传输通道：命名管道/命令文件/钩子），或确认"必须人工按 `Ctrl+Shift+C`"并把该结论写实。
  判定产物 `docs/plan/2026-09-18-p0-real-machine-execution-record.md`：逐项只能写「有证据 / 未覆盖 / 仅趋势」；生产链一律 `identityNotProven` 是有意收紧。
- **T8**：`py AutoTest\dual_perf_baseline.py`（前台；沙箱 `E:\Work\War3_AutoTestSandbox` 缺游戏与两张图 ⇒ 有界重试 + 如实记录）。
- **M2-3（主线程亲办）**：`War3NoteLivePaletteMotion` / `War3NoteDrawTimePoseMotion` / `War3NoteSubmittedPaletteMotion` 迁出；**先补 `FROZEN_M2_3`**（这三个符号不匹配预算门禁命名族，不补就是漏检）；legacy 参考 + 宿主差分 + **≥2 条变异真跑** + 全门禁复跑；产物 `docs/plan/2026-09-18-m2-3-migration-equivalence-record.md`。
- **M2-4/M2-5（富余）**：余下调色板符号继续迁出 + taxonomy 计数对照门禁设计。
- **T7（只读）**：Registry domain 隔离 → Stage13 现状清册与迁移设计。**T2/T3 只复审不实施。**

## 3. 执行协议（主线程每轮）
1. 取队列最前项 → **自己动手**（读码/改码/构建/跑/驱动实机）。
2. 需要长跑或并行辅助件 → 派子代理，然后**显式阻塞等待**（`job_output wait` / 轮询式 `sleep`+落盘检查），等待期间主线程做**不冲突**的重活（例如 M2-3 改码期间不构建、但可读码/写文档）。
3. 实机窗口内**冻结源码树**（No build）——此时主线程只做只读/文档/驱动实机。
4. 构建纪律：Below Normal + `-j2` + 精确目标；**全机同一时刻只一个构建**。
5. 数字必须在**最终树**上取：改动停止 → 冻结 → 全门复跑 → 再写台账（附复跑时间与原始输出）。

## 4. 边界（用户授权之外，仍禁止）
不替换/不覆盖现场 DLL；不 git 写；不称稳定版；不把 CPU 证据当 GPU 提交/像素证据；不把隔离桌面数据当玩家前台性能；不伪造任何未达成的证据。

## 5. 收尾
**07:30 停新工作**；08:00 前交 `docs/plan/2026-09-18-overnight-summary.md`（含：各任务状态、原始证据、未做到项、下一步），并同 checkpoint 更新 changelog。

---

## 附：计划 vs 实际（2026-09-18 主线程晚间对账；本节省略前文，不改动原文）

> 本段由主线程在夜间窗口末追加，供"每轮先读本文件"的后续 agent 直接看到结果，避免按已过期的任务状态重复劳动。
> 所有数字均为**最终冻结树**上主线程实跑所得（`ninja -C build32 -n` = no work；现场 DLL `A0A51AF2…` 未替换）。

| 计划任务 | 实际结果 |
| --- | --- |
| ③ M2-3 motion 三函数迁出 | **完成 + 已验收**（FROZEN_M2_3 先补；legacy 参考 + 宿主差分 + 2 条变异真跑 + 全门禁复跑） |
| ④ M2-5 前置（taxonomy 抽纯函数） | **完成 + 已验收**（新模块 `war3_palette_taxonomy_emission.*`；34 字段逐点等价；新增 FROZEN_M2_5） |
| ④ M2-4（清册 A1–A8） | **完成 + 已验收**（8 符号/9 定义；新增 FROZEN_M2_4 补判定 3 盲区） |
| ④ M2-5 余项（B4 迁出 + 表 B 其余裁定） | **完成 + 已验收**（B1/B2/B5/B11 判**不可做**并给出源码锚理由，未硬做） |
| ④ A9/A10 死代码裁定 | **完成 + 已验收**（A10 删除 / A9 迁出；两条变异均证明**预算门禁无能力**） |
| ⑤ Registry domain 隔离 | **第一块完成 + 已验收，整体未完成**（显式 domain + 8 处 fail-closed owner-check；D1/D3/D4/D5/D7 未动） |
| ⑤ 域拒绝计数对外出口 + 证据补强 | **完成 + 已验收**（3 条变异补跑并关闭缺口；出口走 `war3_perf_report.html` 内嵌 JSON） |
| ④ T7 Registry→Stage13 只读勘察 | **完成**（勘察文档；Stage13 未开始） |
| ④ T2/T3 只复审不实施 | **完成**（复审结论 + 建议裁定；未改一字节） |
| ① T1 实机对象级取证 ×2 | **未覆盖** —— 三种 `-loadfile` 形式均进不了图；沙箱 `preflight_instance_pool` 实测 `ok=false`（缺游戏）。**两条解锁路径已文档化到可执行粒度**：(A) `option-a-manual-evidence-procedure.md`；(B) `option-b-sandbox-provisioning-checklist.md` |
| ② T8 双图基线 | **未覆盖（已做有界重试）** —— 实测 `py AutoTest/dual_perf_baseline.py` 两段均 `ok=False stage=launch`、指标全 0；**注意该脚本失败时仍 EXIT=0** |
| ⑤ 07:30 停新工作 / 08:00 前交总结 | **总结已提前交付**（`2026-09-18-overnight-summary.md`），并附出站交接等共 11 份文档 |

**冻结树门禁（主线程亲跑）**：预算门禁 冻结 **157** / 站点 165→**112** / 已迁出 **46** / device.cpp **567**；全量静态 **256/256**（含**七条等价门禁**）；宿主域测试 **46/46**；生命周期 **187/187**；meson **84/84**；往返 **CHECKS=1107 FAILURES=0 CERTIFIED(A–F)**；记录器 **23/23**；成本 **PASS(38/38)**；帧证据运行时 PASS；DLL **36,283,128 B / `519AFA69…`**。

**口径提醒（不得误引）**：M1/M2 门禁只打印**断言数下限**（3000000 / 2000000），**不能**据此宣称历史计数（3,098,194 / 10,357,597）已复验；M2-3/M2-5 打印的是**全文件累计** 40,215,908。
