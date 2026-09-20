# 2026-09-17 — 候选冻结记录（内部诊断候选；未部署、未实机、未提交、非稳定版）

> 用途：为「下一轮实机申请」提供可核验的冻结身份。冻结后不得再改 src/AutoTest/meson；
> 任何改动都必须产生新冻结记录并重新过门禁（上级 Q-D：DLL/仪表/资源准入/矩阵任一变化均需新裁定）。

## 2026-09-17 上级裁定与本次修订说明（置顶）

- **依据**：上级（codex `01a02e0b`）2026-09-17 03:05 裁定——**未实机候选不得**写
  「已观察到所有者线程唯一推进＋进度发布一致」；写者调用链审计应表述为「**未发现其他写入路径**」，
  **不是**运行时线程见证。
- **本次改了什么**（逐条锚点）：

| # | 位置 | 旧表述 | 处置 |
| --- | --- | --- | --- |
| C-1 | §2 门禁表「槽位缓存写者审计」行 | 「写者均在主循环/Present 所有者线程族，未发现管道/DXVK/点阴影 worker 写者」 | **已收窄**：结论只能是「**未发现其他写入路径**」（源码调用链审计，**不是**运行时线程见证）；旧表述保留删除线 |
| C-2 | §4 末条 + §5「已证明的」 | 「只能写『已观察到所有者线程唯一推进 + 进度发布一致』」 | **已被上级驳回**：未实机候选**不得**这样写；统一改为「**已完成源码所有者门加固和摘要读取改造；纯判定测试通过。生产生命周期与运行时覆盖待验证。**」 |
| C-3 | §5 结论口径末条 | 「『已观察到主循环线程唯一推进 + 进度发布一致』仍需修复 2 + 实机采样才可写」 | **已被上级驳回**（同属运行时肯定表述）：统一为 C-2 的措辞 |

- **本次只改表述口径**：候选身份（字节数 / SHA-256）、全部门禁结果、旧运行证据、阈值与代码**一字未改**；
  本候选仍**未部署、未实机、未提交、非稳定版**。
- **保留可追溯**：被驳回的旧表述一律保留 ~~删除线~~ 或加「已被上级驳回」标注，**不静默删除**。

## 1. 候选身份

| 项 | 值 |
| --- | --- |
| 主产物 | `build32/src/d3d9/d3d9.dll` |
| 字节数 | **36,253,553** |
| SHA-256 | **`D1C3EBBDFF184AEB908F9A97DB4F9E8EBBA39BCCE96B357ED0CC69502391AAF6`** |
| 源码树 | B 树 `dxvk-v1.22-integration-20260914`（未提交；工作树含既有未提交改动） |
| 构建 | **上级纪律：Below Normal 优先级、exact DLL 目标、最多 `-j2`**。
  **更正**：本记录早期曾把一次 `ninja -C build32 -j4` 写成「遵守 ≤4 线程纪律」，**该表述不正确**——
  `-j4` 是当时用户会话资源上限下的做法，**不构成对上级构建纪律的遵守**；测试通过不能追认该偏差。
  此后所有构建均按 Below Normal + `-j2` 执行并如实记录。 |
| 诊断配置 | 沿用 F855 诊断配置（`warvk_skin_palette_contract_candidate=true`、`warvk_internal_frame_recorder=true`） |

## 2. 门禁（冻结时点的原始结果）

| 门禁 | 结果 |
| --- | --- |
| 构建（**Below Normal + `-j2`**，上级要求的纪律） | exit 0 |
| `ninja -C build32 -n` | no work to do |
| `meson test -C build32 --num-processes 2` | **80/80**，Fail 0 |
| 全量静态 `test_*_static.py` | **247/247**，Fail 0 |
| 内核单测 | T1-T17 全过 |
| S2 差分（可复现） | seedCore=0x5332464600000001 / seedUpper=0x5332464600000002，core 300,010 + upper 300,005 输入，**0 mismatch** |
| 新增线程门禁 | `AutoTest/test_semantic_build_thread_gate_static.py` exit 0（含实施者变异验证 7/7） |
| 新增生产边界测试 | `war3_shadow_build_thread_gate_test` **9/9** EXIT=0 |
| 新增进度生命周期测试 | `war3_shadow_build_progress_test` **86/86** EXIT=0（8 例：正常序列/旧代际拒绝/Complete 后拒绝/未 Begin 拒绝/替换构建/Reset 后旧块不可回写/代际非 0 且严格递增/按值语义） |
| 新增**生产共用**生命周期测试 | `war3_shadow_build_lifecycle_test` **187/187** EXIT=0（11 组：未知所有者两种入口且证明分类不可相加、非所有者、正常推进、Reset 后旧工作不可重发、取消、替换在途构建、**A/B 接线回归 C9/C10/C11**；C8 并发摘要读取含**共同开始握手、交错证据（48198 次中间发布）、30 秒超时保护**，自洽失败 0） |
| 新增对象级证据记录器测试 | `war3_palette_object_evidence_test` **19/19** EXIT=0（预算 64/帧·4096/会话·512 终态含在总额内、链闭合=Recovered 需 sawSubmit+sawDraw、未闭合=Unclosed、从未接住=WindowExpired、ObjectGone、TableFull、弱身份/代际未知单列、**零堆分配**、四帧域分列） |
| 新增解析/缺链静态测试 | `AutoTest/test_palette_object_evidence_analysis_static.py` **61/61**（缺链必须判「未覆盖」；「取到 palette」不得判恢复；子门关但有事件必须报错；头块恒等式 terminalEmitted/sum(closed*)/逐种类/容量上界；认证谓词对齐 drawSource/selectionCleared/liveSawSubmit/未知域带值；会话镜像 bits[10]/[29]） |
| 生产侧采集点 | R 具名原因 POD（56 B，`NotChecked` 语义、子门关时零取值）+ S 最终来源按值（override 清空时记空值+原因）+ D 绘制命令已记录；三点子门短路；采集点门禁 EXIT 0；R POD 生产函数测试 **6/6** |
| **生产往返测试**（真实链路：记录器→转换器→环→导出→解析器） | `war3_palette_object_wire_roundtrip_test` + `AutoTest/test_palette_object_wire_roundtrip.py`（meson test）；**A=Recovered+已认证 / B=StageCompleteUncertified / C=Uncovered / D=空样本未覆盖**，`ROUNDTRIP_VERDICT=CERTIFIED_SPEC_SATISFIED` |
| 转换器与导出头块 | `Kind::ShadowState` + `palette-object/v1` 按冻结 wire 逐位写入；`paletteObject{watchCount,counters}` 始终输出（子门关为 0） |
| 生命周期接线 | arm⇒绑定发射器+清表；freeze/export⇒结算终态；换图⇒重置（新 mapEpoch）；设备代际变化（2 处）⇒清表（防跨 Reset 合并） |
| **A/B 接线回归**（上级 03:30 指定） | A 开始 → A 暂停 → Reset/替换并开始 B → **A 发布/完成均被拒**，且 **B 的全部公开状态逐字段不变**；另加「同一 work 换旧 token」「反向错配」「零代际」「空工作身份」全部被拒 |
| 工作身份接线修复（上级 03:30 指出） | 锁内成对取得 `(buildWork, buildWorkGeneration)` 并在后续成对携带；**发布/完成前先核验仍是当前工作**；完成块**先核验再修改任何对外结果**（原先先写 `m_lastStats`/`m_lastFrame`） |
| 两个新计数导出 | `semanticBuildOwnerUnestablishedRefusedCount`、`semanticBuildDirectAdvanceRefusedCount` 各 7 处链路（进程累计值；**不得相加成总拒绝**） |
| 静态门禁变异验证 | 门禁脚本侧 8 条变异 + 2 次对照全部符合预期（去代际检查、改回 shared_ptr、入口不调用唯一检查、插入第二处判定、统计早写、reader 读 live、Cancel 换 Complete 均被拒） |
| 槽位缓存写者审计 | 完成：~~写者均在主循环/Present 所有者线程族~~ **已被上级收窄** —— 结论只能是「**未发现其他写入路径**」（**源码调用链审计结论，不是运行时线程见证**，也不排除未审计的共享非原子状态，见 §4） |

## 3. 相对上一冻结态（36,000,018 B）新增的内容

1. **四项活跃路径纯计数**（当帧、非原子全局；无准入改动）：append 入口 skinned 分母、canonical 拒绝 skinned 分母、两条 draw-time 路径提交分母。
2. **线程修复（产品代码，两部分均已落地）**：
   - 第一部分（所有者门，**两处**：`ensureLatestFrameBuilt()` 与 `ensureFrameBuiltForContract()`）：
     非所有者/所有者未建立一律只保留 `requestLatestFrameBuild()` 请求语义、不推进分块；
     `semanticBuildOffThreadRefusedCount` 已接入 7 处导出链，另有两个仅访问器的内部计数。
   - 第二部分（**安全进度快照发布**，按上级追加要求实现）：摘要**按值发布、无可变别名**
     （`ShadowBuildProgressState`，无 `shared_ptr`/`make_shared`、**不新增每块堆分配**）；
     帧号 / 发布 revision / **工作代际** / 统计**来自同一次发布**；开始、每块、完成、取消、Reset、
     **替换构建**都更新摘要；**旧代际（含 Reset 后的旧块）无法回写**；
     `snapshot()`/`buildStateSnapshot()` 只读该按值摘要，不再触达 `m_buildWork` 的 live 字段。
     两处推进入口共用**同一份**消费权限检查（`consumePermissionGranted`），规则只此一处。
3. **S2 可复现差分目标**（仅测试）与静态门禁扩展。
4. 文档：线程关系证明、线程修复设计、对象级证据工单（含 Step0 附录）、本冻结记录、申请草案、长期计划修订。

## 4. 本候选**不包含**（不得当作已完成）

- **`OwnerUnestablished`/`DirectAdvance` 两个计数尚未接入导出链**（仅有访问器）；
  实机报告只能列 `semanticBuildOffThreadRefusedCount`。
- **修复 3（drain 具名拒绝原因）未做**。
- 写者审计只覆盖 `s_slotBlendedPaletteCache`；其它与渲染线程共享的非原子状态未审计。
  （措辞按上级 03:05：**未发现其他写入路径**，不得写成"运行时线程见证"或"无竞争"。）
- **对象级证据（Step 1③）已实施并且已跑通生产往返**：子门（默认关、受主门约束）、有界记录器、具名原因 POD、
  三点采集、转换器、导出头块、解析器与往返测试均已落地；`docs/plan/2026-09-17-p0-object-evidence-event-table.md` 为冻结事件表。
  仍需在**实机**验证（本记录不代表实机已覆盖）。
- 当场不可得的域（已按上级口径单列，不用可用字段伪造）：`deviceEpoch` 三点一律 0+`epochUnknown`；
  `manifestFrameSerial`/`manifestPublishRevision` 三点不可得（0+`manifestUnknown`）；`lifecycleIdentity` 三点皆无可靠对象实例生命周期证明
  （0+`identityWeak=true`）⇒ **生产采集路径下 `sameObjectCertified` 恒为 false**（只有合成强身份键才可认证，往返测试场景 A 即如此）。
- `skin::Selection::slotAllocationGeneration` 恒 0 ⇒ 五层证明表的「对象所有权」层仍不闭合。
- ~~按上级口径，即便本候选两部分均已落地，也**只能**写「已观察到所有者线程唯一推进 + 进度发布一致」~~
  —— **该措辞已被上级 2026-09-17 03:05 裁定驳回**：未实机候选**不得**写"已观察到…唯一推进＋进度发布一致"
  （那是运行时肯定表述，本候选只有源码静态判定 + 纯判定测试）。统一措辞为：
  「**已完成源码所有者门加固和摘要读取改造；纯判定测试通过。生产生命周期与运行时覆盖待验证。**」
  同样**不得**写「已建立内存序 / 已消除所有竞争」。
- **对象级有界记录**（依赖上级对对象级工单 §7 的 10 项裁定）。
- **槽位所有权见证**（路线 A/B 均未落地；A5 目前只证明「槽位字节属本帧」）。
- AutoTest MCP 的 `allowControlPlaneSemanticDrain: True` 未改（工单要求不动；安全由边界门保证）。

## 5. 结论口径（冻结时的事实边界）

- 本候选**未部署、未实机、未提交**，是内部诊断候选，**不是稳定版**。
- 已证明的（按上级 2026-09-17 03:05 口径表述）：**所测输入集合零差异**（core 300,010 + upper 300,005，
  SplitMix64 固定种子；**适配层为派生模型验证**，非链接级、非全输入）；路径到达计数；
  **代码层面**非所有者/所有者未建立时不再推进构建
  （~~措辞上限：「已观察到所有者线程唯一推进 + 进度发布一致」~~ **已被上级驳回**；
  统一措辞：「**已完成源码所有者门加固和摘要读取改造；纯判定测试通过。生产生命周期与运行时覆盖待验证。**」，
  **不得**写「已消除竞争」）。**成本不等价**（core 扫描 2→4 趟、适配层丢失容量复用），
  已列为性能回归风险。
- 说明：差分测试中「某分支命中为零」（如 core 的 `NoVertexGroups`/`FallbacksFailed`）**只是观测**，
  **未据以删除任何生产检查**。
- `D286…`（36,004,514 B）与更早的 `7DF26FF7…` 均**只是离线 checkpoint**，不是可运行候选；
  旧哈希授权**不继承**。
- **未证明的**：错误矩阵不再被使用、合法对象正确回退、压力解除后恢复、槽位所有权、
  A5 帧证明前提（需修复 2 才算闭合）、热路径成本等价。
- 因此实机申请里的「（i）构建消费线程」一项，本候选可给出**拒绝面**证据；
  ~~「已观察到主循环线程唯一推进 + 进度发布一致」仍需修复 2 + 实机采样才可写~~
  —— **该表述已被上级 2026-09-17 03:05 驳回**（未实机候选不得写运行时肯定表述）。
  统一措辞：「**已完成源码所有者门加固和摘要读取改造；纯判定测试通过。生产生命周期与运行时覆盖待验证。**」
