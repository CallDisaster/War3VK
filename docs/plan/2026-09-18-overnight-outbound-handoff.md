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
# 出站交接（2026-09-18 夜间结束；主线程撰写）

> 给下一个 agent / 下一班的入口。**先读本文件，再读 `2026-09-18-overnight-summary.md`**（总结是结论，本文件是"怎么接着干"）。
> 树：B 树 `E:\Mycode\Source\Repos\War3MapReforge\Core\Base\Graphics\dxvk-v1.22-integration-20260914`，HEAD `ae890542`，**全部改动未提交**。
> 注意：会话工作目录可能是**另一棵树** `…\Graphics\dxvk`，**不要**把它与 B 树混淆（本夜两次 CWD 事故都源于此）。

## 1. 现状（冻结，可直接接续）

| 项 | 值 |
| --- | --- |
| `ninja -C build32 -n` | **no work**（源码已冻） |
| B 树 DLL | **36,283,128 B / `519AFA6904F47E9C2E168FF0E5E0A17A470B27F9F9E714783D99C0333E397306`** |
| 现场 DLL `E:\Work\Warcraft III\d3d9.dll` | **36,271,456 B / `A0A51AF2BB9091B2C677DC34BEBDF76C02347049A89D0BB2AEF4C37518E52134`**（**不要替换**） |
| 预算门禁 | 冻结 **157** / 站点 165→**112** / **已迁出 46** / device.cpp **567** |
| 门禁全绿（主线程亲跑） | 静态 **256/256** · 域测试 **46/46** · 生命周期 **187/187** · meson **84/84** · 往返 **1107/0 CERTIFIED** · 记录器 **23/23** · 成本 **PASS(38/38)** |
| 残留进程 | **0** |

## 2. 下一步（按优先级；每条都标了前置）

1. **pre-U5 基线采集并登记** —— 前置：能进图（见 §3）。协议已就绪：`2026-09-18-pre-u5-baseline-collection-protocol.md`（两段采样 S-A 稳态/S-B 高压、6 个域计数走 `war3_perf_report.html` 内嵌 JSON、反例门）。**没有它 F3 永远无法执行。**
2. **D4/D5 只读分账**（域维度字节/条目账先"可分别读出"，cap/age 策略保持全局不变）。建议见 `2026-09-18-d1-domain-form-decision-brief.md`。
3. **D1 形态 (b) + D7 合并**（若采纳建议则排在 D4/D5 之后；要求见决策简报 §5）。
4. **补跑三条 owner-check 变异**（删 publish / GC / reset 的 owner-check）；出口块的三条已补跑。
5. **M2-5 表 B 剩余项**（各有前置）：B1/B2 需先外移 `War3SemanticSubmitScope`；B5 需先外移 `device.h` 内嵌类型；B11 等 M3；B6–B10 随各自外层编排片；A8 bounds 族调用点（6 个）。
6. **Stage13**：等 D 项闭合后再推（勘察见 `2026-09-18-t7-registry-stage13-survey.md`）。
7. **解析器 94/94、根读方 55/55**：本夜只有子代理证据，**主线程专门检索后仍未能定位可执行入口**。已查清的事实：①全仓 ripgrep `94/94`/`55/55` **只命中 changelog/plan 文字**，**无任何脚本输出或日志打印**；②更完整命名见 `DEVELOPMENT_CHANGELOG.md:411` —— 可能是「**通用根读方静态**」；③`DEVELOPMENT_CHANGELOG.md:132` 把「全量静态 255/255」与二者**并列**⇒ 二者**不在 `test_*_static.py` 的 256 集合内**。⇒ 接手者应优先用「通用根读方静态」这一命名在 `AutoTest/_archive/`、`AutoTest/artifacts/` 或非 `test_*` 命名脚本中定位，**定位并亲跑前不得引用这两个数字**。

## 3. ①/② 为何卡住（不要重复走死路）

- `YDWE.exe -war3 -loadfile` ⇒ 打开**世界编辑器**（不是游戏）。
- `YDWEConfig.exe -launchwar3 -loadfile <绝对路径>` ⇒ 游戏起但**停主菜单**。
- `war3.exe -window -loadfile <相对路径>`（AutoTest 源码 `war3_autotest_mcp.py:6377-6385` 的权威形式）⇒ 标题 200 秒恒为 `Warcraft III`，**未进图**。
- **未进图 ⇒ `InitializeRuntimeCore()` 不跑 ⇒ 控制面管道不存在**（这是唯一根因，与启动器/路径形式无关）。
- AutoTest 沙箱路线：`preflight_instance_pool` 实测 **`ok=false`**，因 `E:\Work\War3_AutoTestSandbox` **没有游戏安装**；`launch_war3_instance` 支持 `env_overrides_json` 与沙箱内 deploy，**但需先有游戏**。
- ⇒ 只有两条路：**(A) 人工进图后按一次 `Ctrl+Shift+C`**（每会话仅一次）；**(B) 授权把完整游戏供进沙箱**。

## 4. 命令形态（踩过的坑）

- **后台作业里跑 ninja 必须用绝对路径或显式 workdir**：`pwsh` 后台作业默认 cwd = 会话工作区树 ⇒ 相对 `-C build32` 会编译**另一棵树**（本夜发生两次，其中一次作废重跑）。
- 门禁脚本分两类：`test_*_static.py` 可直接跑（我在 `AutoTest` 下逐个跑得到 256/256）；**runner 类脚本需要位置参数**（例：`py AutoTest/test_palette_object_wire_roundtrip.py build32/src/d3d9/war3_palette_object_wire_roundtrip_test.exe`），无参数时 argparse 退出码 **2**（≠ 树回归）。
- 宿主测试 exe 可直接运行并打印 `SUMMARY/N passed`：`war3_palette_object_evidence_test.exe`（23）· `war3_palette_object_evidence_cost_test.exe`（38）· `war3_shadow_geometry_domain_test.exe`（46）· `war3_shadow_build_lifecycle_test.exe`（187）· `war3_frame_evidence_runtime_test.exe`。


### 4.1 七条等价门禁的确切文件名（照此跑，别猜名字）

```
AutoTest/test_device_semantic_predicates_equivalence_static.py          (M1)
AutoTest/test_war3_live_palette_selection_equivalence_static.py         (M2-1/M2-2)
AutoTest/test_war3_live_palette_motion_equivalence_static.py            (M2-3)
AutoTest/test_war3_palette_taxonomy_equivalence_static.py               (M2-5)
AutoTest/test_war3_palette_m2_4_equivalence_static.py                   (M2-4)
AutoTest/test_war3_palette_m2_5b_equivalence_static.py                  (M2-5B)
AutoTest/test_war3_palette_a9_migration_equivalence_static.py           (A9)  <-- 注意有 migration
```

它们**全部**属于 `test_*_static.py` 模式（实测该模式文件数 **= 256**），所以「全量静态 256/256」已经把七条都包含进去了；无需另跑。
**口径提醒**：M1/M2 门禁打印的是**断言数下限**（3000000 / 2000000），不是历史实际计数 ⇒ 不要用门禁输出去宣称「M1 差分 3,098,194」这类历史数字未被回归（本夜已如实登记）。

### 4.2 时间戳不可作为单调证据

本夜观测到**系统时钟向后跳约 15 分钟**（疑似 NTP 校正）：同一命令先后取时间得到 `07:13` 与 `06:58`。因此：
- 进度日志/文档里的时间戳**不构成单调时间轴**，相邻条目"后写数字更小"属时钟校正，不代表顺序或记录异常；
- 判断"耗时/先后"应优先用**产物证据**（文件 SHA、字节、mtime、门禁输出）；
- 用户规定的截止时间以**当前时钟**为准。

## 5. 纪律（不得违反）

不替换**现场 DLL**；实机启动目录 = `E:\Work\Warcraft III`；不 git 写；不称稳定版；不把 CPU 证据当 GPU 提交/像素；不把隔离桌面数据当玩家前台性能；Below Normal + `-j2` 且全机单构建；实机窗口冻结源码树；**数字必须在最终树上取**（停止改动→冻结→全门复跑→再写台账）。

## 6. 固化教训（本夜新增）

1. **DLL 身份**：本树 `war3_perf_monitor.cpp` 用 `__DATE__/__TIME__` 构建戳 ⇒ **同源码重链接不逐字节复现**（同尺寸不同 SHA 是正常的）。登记时写明"最终树最后一次真实构建"，一致性靠「源码逐字节相同 + `ninja -n` no work + 全门禁绿」证明。
2. **分层门禁的必要性**：预算门禁**抓不到值级与模板级改动**（A9 语义改动、A10 模板回流两次变异中它都仍 exit 0）；咬住它们的是**宿主差分**与**文本缺席断言**。
3. **门禁锚点更新要留 provenance**：更新既有门禁锚点前**先把该门禁纳入改动前快照集合**（本夜两个门禁锚点更新后无法回溯 pre-change 字节，见 `2026-09-18-gate-anchor-change-provenance-addendum.md`）。

## 7. 待用户裁定

1. D1 形态（建议：暂缓 (b)、先 D4/D5）；2. A9/A10 落地方式是否认可；3. ①/② 解锁选 (A) 还是 (B)。

## 8. 文档索引（本夜产出；按阅读顺序）

| 文档 | 一句话用途 |
| --- | --- |
| `2026-09-18-overnight-outbound-handoff.md`（本文） | **接续入口**：现状、下一步、死路清单、命令坑、教训 |
| `2026-09-18-overnight-summary.md` | **结论**：最终树数字、逐任务「有证据/未覆盖」、三条工程结论、诚实缺口、验证归属 |
| `2026-09-18-overnight-progress-log.md` | **过程**：逐轮台账（根因判定、每步验收数字、5 次自我纠正、时钟异常） |
| `2026-09-18-overnight-execution-plan.md` | 原计划 + 末尾**计划 vs 实际对账** |
| `2026-09-18-pre-u5-baseline-collection-protocol.md` | **F3 前置协议**：字段、两段采样、判定措辞、反例门 |
| `2026-09-18-d4-d5-readonly-accounting-spec.md` | **下一块实施规格**：只读分账（含 5 条红线与守恒判据） |
| `2026-09-18-d1-domain-form-decision-brief.md` | D1 形态 (a)/(b) 决策简报（含我的建议与理由） |
| `2026-09-18-t2-t3-review-verdict.md` | T2/T3 只读复审结论与建议裁定 |
| `2026-09-18-option-a-manual-evidence-procedure.md` | **①(A) 解锁**：人工 Ctrl+Shift+C 步骤 |
| `2026-09-18-option-b-sandbox-provisioning-checklist.md` | **①(B)/② 解锁**：沙箱供给清单（含图来源与目标路径） |
| `2026-09-18-t8-bounded-retry-record.md` | ② 的一次失败重试原始记录（**不含性能结论**） |
| `2026-09-18-gate-anchor-change-provenance-addendum.md` | 两个门禁锚点更新的 provenance 补记（含不可恢复声明） |
| `2026-09-18-m2-3/-m2-4/-m2-5-taxonomy-extraction/-m2-5-remainder/-m2-5-taxonomy-and-remaining-scope/-a9-a10-dead-code-ruling/-registry-domain-isolation/-registry-domain-count-export` 等 | 各交付块的记录 + `*-full-gate*.log` + `*-mutation-raw-output*.log`（原始输出） |
| `2026-09-18-t3-readonly-audit-record.md` | **停止点后的新增交付**：T3 只读审计合并件（缓存访问面 9 处 / 写面 3 确证 / 读面 6 枚举 / 发布协议 / 2026-09-17 线程闸门 + **执行证据 9/9 与 187/187** / 三个无调用者函数 / 三项未闭合 / 覆盖边界） |
（另：`2026-09-18-t2-t3-review-verdict.md` 已在停止点后**新增 §6 更正**——T3 的结构性修复已于 2026-09-17 落地，读该文件时**务必看 §6 而非 §3**） |
| `2026-09-18-dead-code-candidates-brief.md` | **停止点后新增**：三个（现为**两个**）无调用者函数的决策简报；含**更正节 §7**（我漏检 `CopyBlendedPaletteBytesBySlotIndexExact`）与**命名不一致根因 §9** |
| `docs/agent-history/DEVELOPMENT_CHANGELOG.md` | 15 条 2026-09-18 条目（含停止点后新增的 T3 更正条目），均标「未提交 / 未部署 / 未晋升稳定」 |

**入口建议**：先读本文 → 再读 summary 的 §1（最终树数字）与 §4（诚实缺口）→ 若要动手，按 §2 顺序（当前第一优先是 pre-U5 基线采集，协议已备）。
