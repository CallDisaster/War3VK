# 2026-09-17 — P0 对象级证据工单（有界对象级记录 / 槽位所有权 / 线程关系）

- 性质：**方案稿（设计 + 工单），未落地任何代码**。本文只新建此一个 `.md`，
  不改 `src/`、不改测试、不改 `meson`，不构建、不部署、不启动游戏、不做 git 写操作。
  **修订版 R2（2026-09-17）**：按上级十项裁定与「实施前必须改好的关键点」就地修订，被推翻的旧表述保留删除线可追溯（§R.1 / §R.2 / §R.3）；修订仍只改本文件，未改源码/测试/meson，未构建、未部署、未启动游戏、无 git 写。
  **修订版 R3（2026-09-17 03:05 上级裁定）**：修正 §2.2 的对象级事件预算矛盾——**批准的是总计最多 4096 条/会话，512 条终态必须从该总额中预留，不得额外增加**；并把「**分析器必须识别缺链**（事件丢失 / 淘汰 / 饱和必须可判定）」写成门禁要求（§R.5）。本次同样只改本文件，未改源码/测试/meson，未构建、未部署、未启动游戏、无 git 写。
- 目标树：`dxvk-v1.22-integration-20260914`（B 树）。本文所有 `file:line` 均为该树
  2026-09-17 只读观测值；**行号会漂移，一律以"函数名 / 唯一子串锚"为准**（见 §8）。
- 授权链：上级（codex 线程 `01a02e0b…`）裁定记录见
  `docs/agent-history/DEVELOPMENT_CHANGELOG.md` 顶部「2026-09-17 08:2x — 上级审查裁定记录」条目。
  本文只承接其中 Q-A 与"下一会话第一步顺序"④：「单独提出资源键诊断方案（优先有界对象级记录），
  暂不放宽 `IsContractUnitCandidate`」。
  本轮（修订 R2）上级又给出**十项裁定**与 7 条「实施前必须改好的关键点」，逐条落点见置顶 §R.1 / §R.2；被推翻的旧表述处置见 §R.3。**本工单未获实机开跑授权**，任何 Step 1 之后的实机仍需新冻结记录 + 上级裁定。
- 前置文档（本文不重复其结论，只在其上推进）：
  `docs/plan/2026-09-16-palette-gap-fix-workorder.md`（Gap A）、
  `docs/plan/2026-09-17-gapb-confirmation-strengthening-design.md`（Gap B S0-S3）、
  `docs/plan/2026-09-17-gapb-resource-key-fix-workorder.md`（资源键四键失配）、
  `docs/plan/2026-09-17-active-path-palette-instrumentation-workorder.md`（活跃路径计数）、
  `docs/plan/2026-09-17-p0-real-machine-execution-record.md`（P0 三轮实测）、
  `docs/plan/2026-09-17-p0-thread-relationship-proof.md`（§4 的强制前置：off-thread drain 已存在）。

---

## 2026-09-17 上级裁定与修订说明（置顶；本工单修订版 R2 → R3）

- **修订依据**：上级（codex 线程 `01a02e0b…`）本轮给出的**十项裁定**与
  「实施前必须改好的关键点」7 条；上一轮 08:2x 的 Q-A..Q-E 裁定记录仍可在
  `docs/agent-history/DEVELOPMENT_CHANGELOG.md` 顶部查到。
- **修订范围**：只改本文件。**未改 `src/`、未改测试、未改 `meson`，
  未构建、未部署、未启动游戏、无 git 写操作。**
- **可追溯性**：被推翻 / 收窄的旧表述一律保留并加 ~~删除线~~ 或「已被上级驳回」标注（见 §R.3），
  **不静默删除**；章节号沿用原编号，裁定内容就地写入对应章节。
- 本工单仍是**方案稿**，不是实施记录；文中「已裁定」只表示**授权范围**，不表示已落地。

### R.1 十项裁定与逐条落点（裁定 1..10 ↔ 原 §7 问题 1..10）

| # | 上级裁定（已裁定） | 落点 |
| --- | --- | --- |
| 1 | 子门**批准进入 `RecorderConfiguration`**：默认关、受主门约束、effective configuration 可见；**不得在 shadow-core 内另造隐藏 env 入口** | §2.4、§2.3、§7-1、§6 Step 1 |
| 2 | **批准复用 `Kind::ShadowState` + label `palette-object/v1`**（不新增 `Kind=19`、不改既有 wire 布局）；须冻结字段映射、阶段/原因枚举及**解析测试** | §2.1、§2.3、§6 Step 1、§8.11 |
| 3 | 后随观察表**有条件批准**：必须解决**身份、状态机、保留与容量**；**不得第一次 fallback 后即删** | §2.5、§5.2、§5.3 |
| 4 | **本轮不批准新增矩阵扫描哈希**；只可记录**已算好**的摘要及其布局/来源，且**摘要 ≠ 所有权证明** | §2.1、§2.3、§2.6、§5.4 |
| 5 | 所有权：**先批准 B 的有界观察，不批准 A 改准入**；B 只能记录重写/读取关系，**不能凭区间重叠宣布错误矩阵被消费** | §3.2、§3.3、§3.4、§5.3、§6 Step 3/4 |
| 6 | `key.owner` **保持既有设备/记录上下文语义，不填 part 指针**（part 放载荷）；取不到 owner 记 0 并标 unknown，**不得据此强关联** | §2.3、§7-6 |
| 7 | **批准复用现有 pipe/export，不新增 JAPI**；不扩张玩家 API、不另造导出系统 | §2.4、§7-7、§5.4 |
| 8 | **原则批准申请实际客户区 1902×963**，仅限**结构观测**；具体租约待冻结后审批；**不能代替 2560×1440 性能/视觉基线** | §5.5、§6 Step 1 |
| 9 | 取证主门**有条件批准 CPU-only 配置**；不得因主门开启而隐式启用**图像历史环 / GPU 输入池**；须冻结**实际总内存**与导出预算 | §2.4、§2.2、§6 Step 1 |
| 10 | 证明口径**要求统一修订，不能只沿用旧 §8①**：必须**分别**写清「缓存标签 / 实际读取的 arena 字节 / 对象所有权 / 提交 / 绘制」各自证明到哪一步 | §5.6（新）、§3.5、§7-10 |

### R.2 「实施前必须改好的关键点」与落点

| # | 关键点（必须落进文档） | 落点 |
| --- | --- | --- |
| K1 | **身份不能只靠四元组**：rawcode 是类型、handle/指针可复用 ⇒ 关联须含**会话 + map/device epoch + 可证明的对象/模型生命周期身份**；缺失即**弱身份**，不得用于跨帧恢复验收。**当前帧 / manifest 帧 / native palette frameTag 必须分列**，不能任取一个填 `frame` | §2.1、§5.1、§5.3 |
| K2 | 「取到 palette」≠「阴影恢复」：区分「拒绝 → 替代 palette 可用 → **实际提交/绘制关联** → 压力解除后状态」；**超时 / 对象消失 / 容量耗尽 / 事件丢失必须是不同终态**；证据不足是「未闭合」。**恢复不必强制回到 `ArenaSlot`** | §2.5、§5.2、§5.3 |
| K3 | **预算有界 ≠ 证据一定保留**：共享环仍会淘汰拒绝事件 ⇒ 冻结探测次数、生命周期、阶段配额、**终态保留预算（从 4096 总计上限中预留 ≤512，不得额外增加；§R.5）**，并记录**丢失/淘汰/饱和**且**分析器必须据此识别缺链**；开放寻址删除不得破坏碰撞链；**TLS 表 + 全局 watch count 不能叫「表空零成本」**，优先同所有者状态，并**测量关闭与开启成本** | §2.2、§2.5、§6 Step 1 |
| K4 | 路线 B 必须区分两类「写」：`BindingRead`（读 native arena 后更新插件缓存）≠ 游戏重新写 arena；`nodePtr` 与 part 不可直接比较 ⇒ 记录**事件顺序、来源域与可用 write serial**；「同帧两次写且区间相交」最多是**调查线索** | §3.3、§3.4、§5.3 |
| K5 | **删除「先出现 R3 才允许调查所有权」的前置**：所有权错误恰可能在 **A5 全通过**时发生 | §3.1、§6 Step 3 |
| K6 | **不要再给不可达路径装仪表**：旧运行该路径 `resolved=0` ⇒ 必须先说明新证据覆盖**哪条实际路径**；仍为零就明确写「未覆盖」；**不得为产生样本放宽 `IsContractUnitCandidate`** | §1.5、§6 Step 0/1、§0.2 |

### R.3 旧表述处置（保留可追溯：驳回 / 收窄 / 替换）

| 旧表述（位置） | 处置 | 新表述落点 |
| --- | --- | --- |
| 「对象身份（主键）：四元组组合」（§2.1） | **已被上级驳回**（四元组不足以对抗 handle/指针复用；rawcode 只是**类型**） | §2.1 八元组 + 弱身份；旧行保留删除线 |
| 「子门放 `RecorderConfiguration`，**或**在 shadow-core 内独立读 env」（§2.4、§7-1） | **已被上级驳回**后者（禁止隐藏 env 入口）；前者**已批准** | §2.4 |
| 「复用 `Kind::ShadowState` … **还是要**正式扩到 `Kind=19`」（§7-2） | **已裁定**：复用批准；~~扩 `Kind=19` 选项~~ | §2.3 |
| 「后随观察表…第一次命中后移除」（§2.5 旧文） | **已被上级驳回**（第一次 fallback 后即删会使「压力解除后恢复」不可验证） | §2.5 状态机 + 终态 |
| 「后随观察表是否属于已批准范围」（§7-3） | **已裁定**：有条件批准，条件即身份 / 状态机 / 保留 / 容量 | §2.5 |
| 「是否允许记录字节内容哈希」（§7-4） | **已裁定**：本轮**不批准新增矩阵扫描哈希**；可记录已算好的摘要及其布局/来源，**摘要 ≠ 所有权证明** | §2.1、§2.6 |
| 「路线 B 的区间相交即反例」（§3.3、§5.3） | **已收窄**：相交只是**调查线索**；B 不得宣布「错误矩阵被消费」 | §3.3、§3.4、§5.3 |
| 「先做路线 A 还是 B？」（§7-5） | **已裁定**：先批准 **B 的有界观察**；**不批准 A 改准入** | §3.2、§3.4 |
| 「`event.key.owner` = 记录者身份（建议 = renderablePart 或 0）」（§2.3） | **已被上级驳回**（owner 保持既有设备/记录上下文语义，不填 part 指针） | §2.3 |
| 「是否新增 JAPI `warvk:v1` 出口」（§7-7） | **已裁定**：**不新增 JAPI**，复用现有 pipe/export | §2.4 |
| 「本工单**不预设**任何分辨率」（§5.5） | **已被上级部分推翻**：**原则批准申请 1902×963**，仅限结构观测 | §5.5 |
| 「§7-10 沿用 `gapb-confirmation-strengthening-design.md` §8① 的现有措辞」 | **已被上级驳回**：必须统一修订，分别写清五层各自证明到哪一步 | §5.6 |
| Step 3 前置「必须先实测到 R3 `SlotRangeStale` 且无替代来源」 | **已被上级驳回**（所有权错误可能在 A5 全通过时发生；不得以此设前置） | §6 Step 3 |
| §2.5「表空即零成本」/ TLS 表 + 全局 watch count | **已收窄**：该表述不能当零成本结论；优先同所有者状态，并**测量关闭与开启成本** | §2.2、§2.5 |
| §2.2「对象级事件 ≤4096 条」＋「终态 ≤512 条（与拒绝事件 4096 条**分开计**）」 | **已被上级驳回**（2026-09-17 03:05）：批准的是**总计最多 4096 条/会话**，512 条终态**从该总额中预留**，**不得额外增加** | §R.5、§2.2 |
| §2.2/§2.4「有配额即有证据」/ 分析器只解析不判链 | **已被上级驳回**：配额**不保证**共享环里的旧证据不被淘汰；**分析器必须识别缺链**（事件丢失 / 淘汰 / 饱和必须可判定） | §R.5、§2.2、§2.4、§6 Step 1 |

### R.4 本次修订未闭合项（不得当成已证明）

1. 全部源码锚点仍是 2026-09-17 的**只读观测**，行号会漂移，未逐行重核。
2. 新证据**覆盖路径**仍待 Step 1 落地后由实机确认；旧 P0 轮该路径 `resolved=0`，
   因此本修订只能写「**未覆盖**」（§1.5）。
3. 后随观察表的**终态保留预算与淘汰计数**已给出上限设计，但具体常量、实测淘汰率、
   关闭/开启成本量级均**待实施后测量**（附录 B）。
   **R3 补充**：终态 512 条是**从 4096 总计上限中预留**的份额（§R.5 A-1）；且「缺链」必须由分析器**可判定**（§R.5 A-2/A-3）。两项均属实施前必须冻结的未闭合项。
4. 「实际提交 / 实际绘制」两级证据的**采集点**尚未在源码中逐点核实（附录 B）；
   核实前 §5.2 的第 3、4 条只能写成「要求」，不能写成「已具备」。
5. 1902×963 只是**结构观测租约申请**；性能/视觉基线替代仍缺上级审批（§5.5）。
6. §7 的十项问题已在 §7 就地标注裁定结果，但**具体文案**（冻结常量、测试名、枚举表）
   仍需 Step 1 实施时一次性冻结。

### R.5 2026-09-17 03:05 上级裁定与本次修订（预算总额口径 + 缺链门禁）

**依据（上级原话要点）**：① 本工单原稿把预算写成「4096 条拒绝事件之外**再加** 512 条终态」——
**批准的是总计最多 4096 条/会话，512 条终态应从该总额中预留，不能额外增加**；
② **配额不能保证共享环里的旧证据不被淘汰；分析器必须识别缺链**（事件丢失 / 淘汰 / 饱和必须可判定）。

| # | 旧表述（位置） | 处置 | 新表述落点 |
| --- | --- | --- | --- |
| A-1 | 「**对象级事件占环上限 ≤4096 条/会话**」＋「**终态事件 ≤512 条/会话（与拒绝事件 4096 条分开计）**」⇒ 实际合计可达 **4608** 条（§2.2） | **已被上级驳回**：4096 是**总计**上限，512 终态**从总额中预留** | §2.2「对象级事件占环上限（总计）」+「终态保留预算（从总额中预留）」 |
| A-2 | 有配额即说明证据保留；丢失计数器只是"可选诊断" | **已收窄**：配额只约束**写入量**，**不保证**共享环里已写入的旧证据不被淘汰（淘汰是既有行为） | §2.2「配额 ≠ 证据保留」/「链完整性四元组」两行 |
| A-3 | 分析器只做 `effectiveConfiguration` 键集与字段解析，**不判定证据链是否完整** | **已被上级驳回**：**分析器必须识别缺链**，且写成 Step 1 硬门禁 | §2.4「缺链判定（分析器门禁）」+ §6 Step 1 门禁 |
| A-4 | §2.4 导出预算只写「额外受 §2.2 的对象级事件 ≤4096 条约束」 | **已收窄为总计口径**（拒绝 + 阶段 + 终态合计 ≤4096，终态在其中预留 ≤512） | §2.4 大小上限条目 |

- **本次只改预算与缺链的口径表述**：4096 / 512 两个数值本身不变，Step 顺序（§6）与五层证明口径（§5.6）不变，**旧运行证据一字未改**。
- **记账口径（本次冻结，按上级原话）**：**总计上限 4096 条/会话**，其中**终态预留 ≤512 条，且终态本身也计入 4096**；
  非终态（拒绝 + 阶段 + 各来源丢弃）可用额度 = 4096 − 实际终态写入数，**不另设独立配额**。
  台账按「总计 4096 / 其中终态 ≤512」两列记账，**任何两列相加都不得超过 4096**（禁止出现 4608 一类合计）。
  该冻结不改变 4096 / 512 两个数值本身，也不再列为未闭合项。

---

## 0. 范围与禁止项

### 0.1 范围内（本工单定义的三步，顺序不可交换）

1. **先记录同一对象的实际查询键、写入键、资源代际、发布时机与拒绝原因**（§2 设计）。
2. **确认缺失来自哪里**（§2.5 的"后随观察"机制 + §5 的验收口径）。
3. **再形成默认关闭的 dev 候选**（§6 Step 4）；dev 门不是安全检查的替代品。
4. **覆盖路径声明**（本轮裁定 K6 新增）：在写任何仪表之前，必须先说明新证据覆盖**哪条实际路径**（§1.5）；
   旧 P0 轮该路径 `resolved=0` ⇒ 若新轮仍为 0，报告必须写「**未覆盖**」，不得写"没有错误矩阵"。

另含两项必须先闭合、否则第 1 步的证据不可用的问题：**槽位所有权**（§3）与
**构建线程/写入线程同线程证明**（§4）。

### 0.2 范围外（本轮明确禁止）

| 禁止项 | 依据 |
| --- | --- |
| 为让 `resolved>0` 而放宽 `IsContractUnitCandidate` | 上级 Q-A 明确**不批准**；正确顺序是"先记录 → 确认缺失来源 → 默认关闭 dev 候选" |
| 任何形式的无界取证（全量逐对象逐帧、无容量上限、无每帧上限、无失败即关闭） | 上级 Q-B"不开大容量取证"；AGENTS.md 32 位地址空间纪律 |
| 用 `DXVK_WAR3_SEMANTIC_CONTRACT_CAPTURE_PERIOD` 作为本工单的采样预算 | 上级已作废"纯 env trace 零风险"表述：该开关会改抓取频率与时序，**必须独立冻结预算** |
| 改动 caster 准入、槽位所有权语义或矩阵来源仲裁 | 本工单只出方案；语义改动需独立候选与上级裁定 |
| 把"接线"写成"已证明"；把候选/纯证据写成稳定更新 | AGENTS.md 文档维护规则；上级 Q-E |
| 放松任何既有验收口径（含三处 Gap B 证明口径修正） | 上级 Q-C：① arena vs slot cache 的同帧标签、② `>=`→`==` 未新增排除证明、③ `BindingMiss`/`SnapshotFrameStale` 不在同一分母 |
| 新增 `Kind = 19` 或改动既有 wire 布局 | 上级裁定 2：**批准复用 `Kind::ShadowState` + label `palette-object/v1`**，不新增 Kind、不改 wire |
| 在 shadow-core 内另造隐藏 env 入口 | 上级裁定 1：子门**必须进入 `RecorderConfiguration`**，默认关、受主门约束、effective configuration 可见 |
| 新增 JAPI `warvk:v1` 出口或另造导出系统 | 上级裁定 7：**只复用现有 pipe/export**，不扩张玩家 API |
| 本轮新增矩阵扫描哈希 | 上级裁定 4：**不批准**；只可记录已算好的摘要及其布局/来源，**摘要 ≠ 所有权证明** |
| 改槽位所有权准入（路线 A）或据区间重叠宣布错误消费 | 上级裁定 5：**不批准 A 改准入**；B 只能记录重写/读取关系 |
| 因主门开启而隐式启用图像历史环 / GPU 输入池 | 上级裁定 9：主门**有条件批准 CPU-only**；必须冻结实际总内存与导出预算 |
| 用 1902×963 结构观测代替 2560×1440 性能/视觉基线 | 上级裁定 8：仅限结构观测，具体租约待冻结后审批 |
| 为产生样本放宽 `IsContractUnitCandidate` / `IsVisibleDirectGeosetUnitCandidate` | 上级 Q-A + 本轮 K6：仍**禁止**（§1.5） |

### 0.3 纪律

- 本文只写方案与一手位置索引，不写"已实施/已验证/已修复"。
- 不确定处一律写 **待验证**，并进入 §8 附录 B 汇总。
- 本工单**不覆盖** S4 未完成边界：只做 S0-S3 时快照字节仍可能来自 FROZEN 携带的旧槽位，
  报告不得宣称"快照路径已排除陈旧字节"。

---

## 1. 问题定义：为什么聚合计数不足以证明/否证错误矩阵

### 1.1 一次具体失败链（代码路径 + 实测数字）

以 P0 实机三轮（`docs/plan/2026-09-17-p0-real-machine-execution-record.md`）中
合同 OFF 轮的真实形态为例。对**某一个**皮肤化单位对象：

1. 引擎本帧没有更新 `RenderablePart + 0x08`
   （`RenderablePartFieldOffsets::StagePresetSpanBaseIndex`，读者在
   `war3_shadow_renderer_core.cpp` 的 `TryBuildRuntimeGroupPalette` →
   `tryEngineDirectPosePalette` lambda 内 `SafeReadU32Fast(renderablePart, …)`）。
2. `FindOrUpdatePaletteSlotCache` 进入 `else` 分支（`currentSlotIndex == 0xFFFFFFFF`
   或 `>= 0x3A98`），逐条评估 A0-A5。
3. 若 A0（`QueryRenderablePartPaletteSlot` 绑定命中）失败，
   `g_paletteSlotCacheRejectedStaleCount` 与 `g_paletteSlotCacheBindingMissRejectCount`
   各 +1，函数 `return 0xFFFFFFFFu`。
4. 调用方（`tryEngineDirectPosePalette`）随后在
   `paletteSlotIndex == 0xFFFFFFFF || paletteSlotIndex >= 0x3A98 ||`
   `paletteSlotIndex + requiredCount > 0x3A98u` 处 `return false`，
   **不读 Game.dll 全局 arena**（`kGlobalPaletteBufferRva` 解引用之后的 48×N 解码循环不进入）。
5. 该对象改由下游合法替代来源供出：
   - ① 若在 `requiredCount <= 64` 内，先试 `QueryRenderablePartPaletteSnapshot`；
   - ② 否则用共享纯计算内核，以 `pose.matrixPalette`（PoseRegistry 已发布姿态）
     构建分组矩阵（`render/war3_runtime_group_palette_kernel.h` 的
     `TryBuildRuntimeGroupPaletteKernel`，适配层在 `war3_shadow_renderer_core.cpp`）。
6. 若两条替代来源都失败，该对象本帧**不被投影**（阴影缺失）。

实测的三轮累计值（同一 DLL `8CECC495…`，客户区 1902×963）：
Gap A 陈旧槽位拒绝 `31,881 → 88,926 → 91,039`（Δ21 = 57,045，Δ32 = 2,113）；
`ServedAfterConfirm` 全程 0；Gap B 三轮全 0；替代来源计数采样瞬时全 0。

### 1.2 现有计数器能回答什么、不能回答什么

| 问题 | 现有计数器 | 能否回答 |
| --- | --- | --- |
| 某次拒绝**属于哪个对象**？ | 无（只有进程累计 `uint64_t` 原子） | **不能**。拒绝计数是分母，不含 `renderablePart`/`runtimeModelPtr`/`jHandle`/`rawcode` 任一字段 |
| 该对象**之后是否经合法来源投影**？ | `g_paletteSlotCacheProducerSnapshotFallbackCount` 等为进程累计 | **不能**。无法把某一次拒绝与后续某一次替代命中关联到同一对象 |
| **压力解除后同一对象是否恢复**？ | 只有趋势（Δ21/Δ32） | **不能**。趋势无法区分"同一批对象恢复"与"旧对象消失、新对象顶上" |
| 慢路径键 miss 是**哪一档**？ | 4 键 miss 的形状可由 manifest 反推（资源键工单 §1.2） | **部分**。属于代数结论，仍不含对象身份 |
| 供出的 arena 字节**属于谁**？ | 无 | **不能**（§3 的槽位所有权缺口） |

### 1.3 为什么"拒绝计数增长"不是证明

- 计数增长只证明**路径被到达**（上级 Q-B：「这四项只证明路径到达与提交数量」）。
- 细分拒绝之和 == 聚合 `RejectedStale` 只证明**分母自洽**，不证明任何一次拒绝的后果。
- `ServedAfterConfirm == 0` 只支持"该运行未观察到确认后命中"，
  不能定性为结构性死代码（上级 Q-E 对 `CanonicalReadyCount=0` 的同类修正）。
- 更要紧的是**方向性风险**：若把"拒绝计数上升"当成"发现了陈旧矩阵"，
  则可能把"合法对象被误杀"读成"防护生效"。两者在聚合面上不可区分。

### 1.4 结论

要能回答 §1.2 的前三问，必须引入**对象级、有界、可关联**的记录：
以对象身份为主键，记录该对象的查询键、命中键、资源代际、拒绝原因，
并在其后**一次性**记录它是否被某个**具名来源**接住。这正是 §2 的设计目标。

### 1.5 证据覆盖路径（本轮裁定 K6：不得给不可达路径装仪表）

仪表只能证明它**实际被执行的路径**。因此本工单在 Step 1 之前必须先回答"新证据落在哪条路径"：

1. 本工单的全部记录点只挂在 `FindOrUpdatePaletteSlotCache` 的 `else` 分支（"记忆槽位复核"路径）
   与下游来源选择链 `tryEngineDirectPosePalette` / `TryBuildRuntimeGroupPalette` 上；这是**唯一**的数据来源。
2. 旧 P0 三轮（`2026-09-17-p0-real-machine-execution-record.md` §2.3）里，**shadow-core 蒙皮 caster 路径整轮未运行**：
   语义 core 构建完成（`semanticCoreFrameSerial == ManifestFrameSerial`、`publishRevisionLag=0`、`buildInProgress/Pending=0`），
   但 `considered=2 / skippedNoGeoset=2 / skippedResourceMiss=2 / **resolved=0**` ⇒ draws 为空 ⇒ `submittedDrawCount=0`；
   资源四键全 miss 发生在 `war3_shadow_renderer_core.cpp` 的资源查找点（属**代码问题，非 env 可解**）。
3. ⇒ **本工单要采集的对象级事件，恰好覆盖这条旧轮为 0 的路径。** 因此修订后的报告口径是：
   - 新轮该路径仍 `resolved=0` / 无对象级事件 ⇒ 报告必须写「**未覆盖**」，不得写成"拒绝计数为 0 所以没有错误矩阵"；
   - 新轮有事件 ⇒ 必须同时给出"覆盖路径"的当轮证据（resolved / submitted 计数与对象级事件数的对应），否则仍算未覆盖。
4. **禁止的替代**：不得为产生样本而放宽 `IsContractUnitCandidate` 或 `IsVisibleDirectGeosetUnitCandidate`
   （上级 Q-A；§0.2）。资源键修复是独立候选（§6 Step 4），不与本工单合并验收。

---

## 2. 有界对象级记录设计（核心）

### 2.1 记录字段

**对象身份（主键）**：~~四元组组合，不得只用其一。~~ **上述"四元组即身份"的旧表述已被上级驳回**：
`rawcode` 是**类型**、`jHandle` / 指针都可能被复用，四元组不足以承担跨帧身份。
修订后的对象键必须由**会话 + 代际 + 可证明的生命周期身份 + 定位分量**共同构成（裁定 K1）：

```text
ObjectKey = (sessionGeneration, mapEpoch, deviceEpoch,          // 会话与代际：必需
             lifecycleIdentity,                                  // 可证明的对象/模型生命周期身份：必需
             renderablePart, runtimeModelPtr, jHandle, rawcode)  // 原四元组：仅作定位与交叉核对分量
```

- `sessionGeneration` = `g_paletteSlotCacheSessionGeneration`（已存在于 core.cpp 匿名命名空间，跨地图会话隔离号）；
  `mapEpoch` = `ShadowModelResourceCache::instance().mapEpoch()`；`deviceEpoch` 当场不可得时记 0 并置 `epochUnknown`。
- **`lifecycleIdentity` 只能取既有、可证明的发布契约，不得现造**：候选是
  `war3::render::skin::Selection` 的 `ownerEpoch / publicationTicket / captureSerial`
  （`war3_skin_palette_selection.h` 自述 "CPU publication contract, NOT an invented native slot lease"），
  或模型注册侧 immutable generation。**哪个来源在记录点当场可得，须在 Step 1 冻结**；两个都不可得 ⇒ `identityWeak`。
- `renderablePart` 是 `FindOrUpdatePaletteSlotCache` 的实际缓存键、A0 的查询键；
  `runtimeModelPtr` 是 pose/资源侧键；`jHandle` / `rawcode` 用于交叉核对。
- 组合哈希（FNV-1a 64）**只能做桶提示**，命中后必须逐字段比较（Stage13 /
  `war3_shadow_runtime_contract.cpp` 既有血统，见 `DEVELOPMENT_CHANGELOG.md` 2026-08-30 条目"摘要只做桶提示"）；
  零哈希不得发布，必须归一为非零桶值。**桶提示哈希不是矩阵扫描哈希**（裁定 4 只禁止后者）。
- **弱身份 `identityWeak`**：下列任一成立即置位，该样本**不得用于跨帧恢复验收，也不得单独作正例/反例**（§5.2 / §5.3）：
  - `jHandle == 0 && rawcode == 0`；
  - `sessionGeneration / mapEpoch / deviceEpoch` 任一为 0 或 unknown；
  - 取不到可证明的 `lifecycleIdentity`。

**帧号：三个不同的"帧"必须分列**（裁定 K1：不得任取一个填 `event.key.frame`）——

| 记录字段 | 含义 | 当场来源 | 禁止混用为 |
| --- | --- | --- | --- |
| `currentFrameSerial` | **当前帧**（渲染 / 构建推进帧） | `RenderState::instance().getFrameIndex()` 或 `renderable.frameSerial` | manifest 帧、palette frameTag |
| `manifestFrameSerial` | **manifest / canonical 语义帧** | semantic core / manifest 当帧记录（旧 P0 用 `semanticCoreFrameSerial == ManifestFrameSerial` 对拍） | current、palette tag |
| `nativePaletteFrameTag` | **native palette 帧戳** | `QueryCurrentPaletteFrameTag()`（`war3_model_hook.cpp` 读 `g_gameBase + 0xBDA4CC`） | current、manifest |
| `boundFrameTag` / `slotRangeMin/MaxFrameTag` | producer 绑定帧 / 逐槽区间帧 | A0 / A5 输出 | 上三者的替代 |

- `event.key.frame` **只填当前帧**（`currentFrameSerial`）；`manifestFrameSerial` 与 `nativePaletteFrameTag`
  走独立载荷字段（§2.3），报告必须三列并排。任何"取其中一个填 `frame`"的写法都已被上级驳回。
- 代际字段：`mapEpoch`、`deviceEpoch`、`sessionGeneration`（定义同 §2.1 对象键）。

**查询键与实际命中键**（这是上级 Q-A 要求的"实际查询键/写入键"）：

| 字段 | 含义 | 现成来源 |
| --- | --- | --- |
| `requiredPaletteCount` | 本次所需矩阵数（`outMaxVertexGroupSlot + 1`，上限 256） | lambda 顶部已算 |
| `currentSlotIndexRaw` | 本帧从 `RenderablePart + 0x08` 读到的原始值（可为 `0xFFFFFFFF`） | 已读局部变量 |
| `rememberedSlotIndex` | `s_paletteSlotCache[i].paletteSlotIndex`（记忆槽位） | 已读 |
| `boundSlotIndex` / `boundGroupCount` / `boundFrameTag` | producer 绑定表（`s_renderablePartPaletteBindings`）记录 | `QueryRenderablePartPaletteSlot` 输出 |
| `slotRangeMin/Max/Missing` | 逐槽字节帧范围 | `QueryBlendedPaletteFrameTagRange` 输出 |
| `hitKind` | 实际产出字节的来源：`ArenaSlot` / `ProducerSnapshot` / `PoseKernel` / `None` | 由调用方分支决定 |
| `hitKey` | `ArenaSlot` → slot 索引；`ProducerSnapshot` → `(part, requiredCount, paletteWriteSerial)`；`PoseKernel` → **只记当场已存在的发布序号**（如 `s_renderablePartPaletteSnapshotSerial` 一类），无序号则记 0 + `digestUnavailable` 位 | 见下方备注 |

> 备注（**2026-09-17 上级裁定 4 修订**）：**本轮不批准新增矩阵扫描哈希**。
> 因此 `PoseKernel` 一类**不得**为取证去哈希 `pose.matrixPalette` 或 arena 字节；
> 只允许记录**已经算好**的摘要（例如 `Selection.hash` 一类既有 CPU 发布摘要），
> 并同时记录其**布局 / 来源**（摘要覆盖什么、算法、谁算的、何时算的）。
> **摘要不等于所有权证明**：摘要相等只支持"同一份已发布拷贝"，不支持"arena 字节属于该对象"。
> 待验证（附录 B）：`PoseKernel` 路径当场是否已有可用的发布序号；若没有，该字段只能是 0 + `digestUnavailable`。
> 原始备注（保留可追溯）：~~`PoseKernel` → `(runtimeModelPtr, pose.matrixCount, 哈希)`；
> 若没有发布序号只能用哈希近似~~ —— 其中"新增哈希"部分**已被上级驳回**。

**拒绝原因**：不复用聚合枚举，而是**A0-A5 逐条**（命名沿用
`docs/plan/2026-09-17-gapb-confirmation-strengthening-design.md` §4.1）：

```text
R0  BindingMiss              A0 失败（含 slotDomainValid 失败、A2 槽位与记忆不一致）
R1  GroupShort               A3 失败（groupCount < requiredPaletteCount）
R2  BindingFrameStale        A4 失败
R3  SlotRangeStale           A5 失败
R4  SnapshotFrameStale       快照存在但帧号确实超差（对齐现有 g_paletteSlotCacheSnapshotFrameStaleRejectCount）
R5  SnapshotUnprovable       当前帧不可读 / snapshotFrameTag == 0（**不计入 R4**，口径见 core.cpp 内注释）
R6  SnapshotSizeMismatch     outPalette.size() != requiredCount
R7  KernelFallbackFailed     两条替代来源都失败（对象本帧无投影）
```

> **分母口径纪律**（上级 Q-C ③）：`R0` 与 `R6/R7` **不属于同一拒绝分母**。
> 对象级记录必须为每条事件带上 `parentCounterDomain` 位，
> 明确它是否计入聚合 `RejectedStale`；报告不得把两侧相加。
>
> **枚举与解析测试冻结（2026-09-17 上级裁定 2）**：本工单**已获批准**复用
> `Kind::ShadowState` + label `palette-object/v1`（不新增 `Kind=19`、不改既有 wire 布局）。
> 批准的条件是**冻结三件事**，并在 Step 1 用非 Python 解析测试钉死：
> 1. **字段映射表冻结**（§2.3 的 `data[12]` / `bits[48]` 逐位含义），不得留"实现时再定"的位；
> 2. **阶段枚举冻结**：拒绝原因 `R0..R7`、替代来源枚举、终态枚举（§2.5）各自编号一次冻结；
> 3. **解析测试**：新增可执行/静态测试，把本工单的 label、kind、`bits[0]` 枚举域、
>    `data` 维度、NUL 终止与"未知枚举必须 fail-visible"钉死（`test_recorder_event_wire_golden.py` **不改**）。

**设备侧对应物**：`d3d9_device.cpp` 侧的同名复核链（Gap A）有独立计数器
（`g_devicePaletteSlotCacheServedAfterConfirmCount` / `…RejectedStaleCount`）。
本工单**先只做 shadow-core 侧**；device 侧是否为同一对象、是否同帧，**待验证**，
不得因为两侧都有计数器就假定同一分母。

### 2.2 有界性（硬预算，实施前冻结）

| 维度 | 取值 | 论证 |
| --- | --- | --- |
| 事件存放 | **复用 `evidence::Ring`**，不新造环形缓冲 | `war3/tools/war3_frame_evidence_core.h` 的 `Ring` 已有 arm/freeze/visitFrozen/容量上限 |
| 环容量 | 沿用既有 profile：内部构建 65536、外部控制 262144（`RecorderProfile`） | 内存已在 arm 时一次性 `make_unique<Cell[]>` 申请，对象级事件**不新增任何堆分配** |
| 单事件字节 | `sizeof(Event)` 按字段计算 = 392 B（`8×4 + 8×4 + 4 + 4 + 32 + 96 + 192`）；`Cell` 另加一个 `atomic_flag` | 实施时加 `static_assert` 固定 |
| 环总内存 | 262144 × ≈400 B ≈ **105 MB**；65536 × ≈400 B ≈ **26 MB**；8192 × ≈400 B ≈ **3.3 MB** | 与既有取证同量级；arm 时已有 `RecorderMemoryAdmission` 拒绝路径 |
| **对象级事件占环上限（总计）** | **≤ 4096 条 / 会话（总计上限：拒绝 + 阶段 + 终态 + 各来源丢弃合计不超过 4096）**（外部 262144 容量的 1.6%，内部 65536 的 6.25%） | 保证对象级事件不会挤掉 draw/caster 证据；**4096 是合计上限，不是"拒绝 4096 + 终态另加 512"**；超出即丢并计数（§R.5 A-1） |
| **每帧对象级事件上限** | **≤ 64 条/帧**（`kMaxDemandFillPerCapture = 64` 同量级先例） | 单帧拒绝风暴（换图/加载）不得刷爆环 |
| **每对象每帧至多 1 条拒绝事件** | 由 §2.5 watchlist 去重 | 同一对象同帧多处尝试只记一次 |
| 触发条件 | **只在拒绝路径记录**；成功/提交侧只在"命中被观察对象"时**按阶段各记一条**（接住 / 提交 / 绘制 / 终态），不逐成功记录 | 满足"只在拒绝路径记录而非全量"；成功路径只做一次键探测（§2.5），且该探测受**探测次数上限**约束 |
| watchlist | 固定 1024 槽、开放寻址、**静态数组、零分配**（`static thread_local`）；**删除必须写墓碑，不得清空碰撞链**（裁定 K3） | 1024 × ≈64 B = **64 KiB**；墓碑不回收则不降容量，回收须保持探测序列正确；**TLS 表 + 全局 watch count 不等于"表空零成本"**（§2.5 第 4 条） |
| **失败即关闭** | 任一步失败（无 session / 环冻结 / 槽位满 / 计数超限）→ 只 `fetch_add` 本地丢弃计数并 `return`，**不抛异常、不加锁、不分配、不阻塞渲染** | 对齐 `evidence::Record` 的 `try/catch(...){lost++; return 0;}` 形态 |
| **探测次数上限** | 每帧成功/提交侧探测 ≤ **256 次**（与每帧事件 ≤64 条**分开**计数） | 裁定 K3：预算有界 ≠ 证据保留；"因探测上限被丢"与"因表满被丢"必须分开计 |
| **条目生命周期** | 单条目最长存活 **≤ 2 个观测段或 ≤ 3600 帧**（先到者为准）；到期发 `expired` 终态事件后再删 | 防止条目在会话内无界累积；与 §5.2 段末判定对齐 |
| **阶段配额** | 每对象每阶段（拒绝 / 接住 / 提交 / 绘制 / 终态）各 ≤1 条事件；同阶段重复只更新条目 | 防止同一对象在同一阶段刷爆环 |
| **终态保留预算（从总额中预留）** | 终态事件 ≤ **512 条 / 会话**，**从上一行的 4096 总计上限中预留**（~~旧稿：与拒绝事件 4096 条分开计，合计可达 4608~~ —— **已被上级 2026-09-17 03:05 裁定驳回**）；环接近饱和时**先丢拒绝、后丢终态** | 裁定 K3：必须为"闭合 / 未闭合 / 超时 / 对象消失 / 容量耗尽 / 事件丢失"保留可判读证据；**预留不等于额外增加**（§R.5 A-1） |
| **丢失 / 淘汰 / 饱和计数** | 至少 5 个独立计数器：`droppedPerFrame` / `droppedPerSession` / `droppedTableFull` / `droppedProbeLimit` / `ringEvictedAfterRecord` | 共享环淘汰是既有行为（`Snapshot.evicted`）；**不得**把"环里有事件"当作"证据一定保留"，报告必须声明本轮淘汰/饱和 |
| **配额 ≠ 证据保留（缺链必须可判定）** | 4096 / 512 只约束**写入量**；**不保证**共享环里已写入的旧证据不被淘汰。上述 5 个计数器**必须**随导出可读，且**分析器必须据此判定「缺链」**：任一非零 ⇒ 该对象当轮**未覆盖** | 上级 2026-09-17 03:05 裁定：配额**不能**保证旧证据不被淘汰；**分析器必须识别缺链**（§R.5 A-2/A-3；门禁见 §2.4、§6 Step 1） |
| **链完整性四元组** | 导出头部必须输出：本会话**对象级事件写入总数**、**终态预留占用**、**5 个丢失计数器**、**环淘汰计数**；分析器据此对账"期望链 vs 实得链" | 缺链必须由**数据**判定，不得由"配额未超"推出"证据完整"；报告须逐对象写「**缺链**」或「**未覆盖**」（§2.4） |
| 独立预算声明 | 上述常量在实施前写死并冻结入台账；**不得**用 `DXVK_WAR3_SEMANTIC_CONTRACT_CAPTURE_PERIOD` 调节采样率 | 上级 Q-A：该开关改抓取频率与时序 |

**禁止的取证形态**（明确不做）：逐帧全对象快照、每次成功供出都记事件、
保存矩阵字节（12 KB/对象）、按对象动态扩容、把 `mapEpoch` 变化前的旧条目留在环里冒充新会话。

**本轮追加禁止（裁定 4 / 9）**：不得为取证新增矩阵扫描哈希；不得为取证开启图像历史环 / GPU 输入池；
不得把摘要相等写成所有权证明。

### 2.3 与既有 `war3::tools::evidence` 的关系：复用，不新造

结论：**复用全部基础设施**，只新增"一种标签的事件 + 一个有界后续观察表"。

复用清单（均有现成定义）：

| 复用对象 | 位置（唯一子串锚） |
| --- | --- |
| `evidence::Event`（`key`/`thread`/`label[32]`/`data[12]`/`bits[48]`） | `war3_frame_evidence_core.h` → `struct Event {` |
| `evidence::Ring`（capacity / freeze / visitFrozen / writers 计数） | `war3_frame_evidence_core.h` → `class Ring {` |
| `evidence::Record(session, event)`（QPC + thread 戳 + try/catch 丢弃） | `war3_frame_evidence.cpp` → `uint64_t Record(uint64_t session,Event event) noexcept` |
| `evidence::Control`（arm/status/trigger/freeze/export/discard） | `war3_frame_evidence.cpp` → `json Control(const json& payload` |
| 导出（流式 JSON、CREATE_NEW、部分文件保留） | `war3_frame_evidence.cpp` → `bool createNew(` |
| 磁盘预算 6 GiB | `war3_frame_recorder_config.h` → `RecorderDiskHeadroom` |
| 内存准入 | `war3_frame_recorder_memory.h` → `RecorderMemoryAdmission` |
| 控制权租约 | `war3_frame_evidence_control.h` → `RecorderControlLease` |

**关键设计决策：不发新 `Kind`，复用 `Kind::ShadowState` + 新 label。**

理由（一手证据）：`AutoTest/test_recorder_event_wire_golden.py` 的
`raise WireError("EventValue", f"kind {ev.kind} out of range 1..18")` 把 kind **钉死在 1..18**；
当前 `enum class Kind` 恰好 18 个值（`PresentBegin=1` … `DirectionalDraw`）。
新增 `Kind = 19` 会同时改动 wire 校验、golden 用例与 schema 面。
既有先例 `d3d9_device.cpp` 的 `std::memcpy(event.label.data(), "skin-selection/v1", 18);`
正是"复用 `Kind::ShadowState` + 独立 label"的成功做法。

> **2026-09-17 上级裁定 2（就地标注）**：**批准**该复用；~~"是否要正式扩到 `Kind = 19`（同时改 wire 校验与 golden）"~~
> 这一选项**已被上级驳回**，本轮不再作为待裁定项。

本工单采用 `label = "palette-object/v1"`（18 字节含 NUL，合法且 `label` 内必有 NUL 终止符）。**2026-09-17 上级裁定 2：已批准此复用**，不新增 `Kind=19`、不改既有 wire 布局；批准条件是冻结字段映射、阶段/原因枚举与解析测试（§2.1）。

**字段映射**（`Event` 的既有容器，不扩结构体）：

```text
event.key.owner     = **既有设备/记录上下文语义**（不填 part 指针；part 放载荷）
                      —— 上级裁定 6：取不到 owner 时记 0 并置 unknown 位，**不得据 owner 做同一对象的强关联**
event.key.frame     = currentFrameSerial（**只填当前帧**；manifest 帧与 native palette frameTag 分列，§2.1）
event.key.mapEpoch  = ShadowModelResourceCache::instance().mapEpoch()
event.key.deviceEpoch = 设备 epoch（当场不可得则 0 并置 epochUnknown；不得猜测）
event.thread        = evidence::Record 自动戳的"消费/记录线程"

data[0]  renderablePart
data[1]  runtimeModelPtr
data[2]  requiredPaletteCount
data[3]  currentSlotIndexRaw        （0xFFFFFFFF 表示 +0x08 本帧未更新）
data[4]  rememberedSlotIndex
data[5]  boundSlotIndex
data[6]  boundGroupCount
data[7]  boundFrameTag
data[8]  currentPaletteFrameTag     （不可读则 0）
data[9]  manifestFrameSerial      （manifest / canonical 语义帧；与 event.key.frame 的当前帧、data[8] 的 native palette frameTag 分列，§2.1）
data[10] hitKind                    
data[11] hitKeyPayload              （ArenaSlot→slot；Snapshot→paletteWriteSerial；PoseKernel→**既有发布序号/摘要，无则 0 + digestUnavailable**；**不为取证新增哈希**，裁定 4）

bits[0]  拒绝/结果原因序号（§2.1 的 R0..R7 或 follow-up 的来源枚举）
bits[1]  jHandle
bits[2]  rawcode
bits[3]  objectKind
bits[4]  groupIdx（int8 提升；-1 记 0xFFFFFFFF）
bits[5]  stage（int16 提升）
bits[6]  slotRangeMinFrameTag
bits[7]  slotRangeMaxFrameTag
bits[8]  slotRangeMissingCount
bits[9]  producerKind（s_renderablePartPaletteBindings 既有字段）
bits[10] paletteSlotCacheSessionGeneration 低 32 位
bits[11] 尝试序号：本次是第几个 TryBuildRuntimeGroupPalette 调用点（1..6）
bits[12] 消费线程 id（与 event.thread 冗余，便于离线对拍）
bits[13] 写入线程 id 见证（§4；未知记 0）
bits[14] 标志位：identityWeak | hadSnapshot | currentFrameUnreadable | readerFaulted
bits[15] 父计数分母域：是否计入聚合 RejectedStale
bits[16] epochUnknown（mapEpoch / deviceEpoch / sessionGeneration 任一取不到）
bits[17] digestUnavailable（当场没有已算好的摘要；裁定 4：不为取证新增矩阵扫描哈希）
bits[18] terminalKind 低 4 位（§2.5 终态枚举；仅终态事件有效，0 = 未判定）
bits[19] reserved（本轮**不得**用作路线 A 的 owner part —— A 未获批准，见 §3.2）
bits[20] followUpDeltaFrames（仅 follow-up / 终态事件使用；否则 0）
bits[21] nativePaletteFrameTag 低 32 位（与 data[8] 冗余，便于离线对拍）
bits[22..47] 保留（必须显式置 0，禁止用作临时用途）
```

**记录点的硬约束**：只允许复制**当场已经读取的局部值**。
不得为取证新增任何内存读取、`Query*` 调用、分配或哈希（裁定 4；§2.5 的一次**键探测**是表查找，不是矩阵哈希）。
这与既有 `skin-selection/v1` 记录点的做法一致（它复用的是 `attemptedPaletteSelection` 快照）。

### 2.4 运行时可解析：哪些出口可 dump，格式与大小上限

| 出口 | 状态 | 说明 |
| --- | --- | --- |
| **env（主门）** | 现成，**本轮有条件批准 CPU-only 用法** | `DXVK_WAR3_FRAME_EVIDENCE=1` / `=0`；解析在 `war3_frame_evidence.cpp` → `recorderConfiguration()`；**主门开启不得隐式启用图像历史环 / GPU 输入池**（裁定 9，见下方"CPU-only 冻结"） |
| **env（子门，新增）** | **已批准进入 `RecorderConfiguration`** | `DXVK_WAR3_FRAME_EVIDENCE_PALETTE_OBJECT=1`：**默认关**、受主门约束（主门关 ⇒ 子门无效）、effective configuration 可见；**禁止在 shadow-core 内另造隐藏 env 入口**（裁定 1） |
| **env（输出目录）** | 现成 | `DXVK_WAR3_FRAME_EVIDENCE_OUTPUT`；未设时默认 `<dll>\\WarVK\\Log\\FrameEvidence` |
| **面板（Ctrl+F1）** | 现成，**只读展示** | `war3/ui/war3_imgui.cpp` → `War3Imgui::drawFrameRecorderPanel()`，标题 `帧取证 (Ctrl+Shift+C)`；并明确写"完全停用取证：启动前设置 `DXVK_WAR3_FRAME_EVIDENCE=0`" |
| **热键（Ctrl+Shift+C）** | 现成 | `d3d9_window.cpp` → `tools::HandleFrameHistoryShortcut(...)` → `TriggerFrameHistoryFromGame()`；自带采集器按 `DefaultRecorderProfile(...)` arm，导出 `incident.json` + `cpu-<pid>-<nonce>-<gen>.json` |
| **控制平面 pipe** | 现成 | `war3_control_plane.cpp` → `if (command == "frame_evidence")` → `evidence::Control(payload)`；动作 `arm/status/trigger/freeze/export/discard`；外部 watcher 见 `AutoTest/frame_history_watch.py`、`AutoTest/run_self_contained_recorder_gate.py` |
| **内部测试 API** | 部分 | `war3_internal_test_api.cpp` 目前只读 `QueryFrameHistoryHud()`；能否承载 evidence 导出命令 **待验证** |
| **JAPI `warvk:v1`** | **已裁定：不新增** | 检索不到 `frame_evidence` 命令；上级裁定 7 批准**只复用现有 pipe/export**，不扩张玩家 API、不另造导出系统。报告中仍**不得**声称 JAPI 已暴露 |

**dump 格式与大小上限**：

- 文件：每个会话一个 `cpu-<pid>-<nonce>-<gen>.json`（`CREATE_NEW`，已存在则失败，
  部分文件保留、重试不覆盖 —— 既有契约）。
- 结构：`{"schema":7, …头部计数…, "events":[…]}`；事件由 `eventJson` 逐个序列化
  （`data` 为十进制字符串数组，`words32` 为 `bits` 数组）。
- 写盘：流式、每 64 KiB flush 一次，**不构造第二棵 nlohmann 树**。
- 大小上限：由 arm 时的 capacity 决定。按 `sizeof(Event)=392 B` 计：
  **8192 → ≈3.3 MB；65536 → ≈26 MB；262144 → ≈105 MB**（JSON 展开后更大，
  既有实现已按此容量工作）。本工单要求实机 **capacity ≤ 65536**，
  并额外受 §2.2 的**总计上限**约束（~~旧稿"对象级事件 ≤ 4096 条"~~ ⇒ **对象级事件合计 ≤ 4096 条/会话，其中终态从该总额中预留 ≤ 512 条**；§R.5 A-4）。
- 磁盘前置：输出目录可用空间 ≥ 6 GiB，否则 arm 直接失败（既有 `RecorderDiskBudget`）。

**CPU-only 取证配置冻结（2026-09-17 上级裁定 9）**：

- 本轮获批的只是**主门的 CPU-only 用法**：`frameEvidence=1` 且 `rawInputs=0`、`local/self-contained=0`。
  `ResolveRecorderConfiguration` 里 inputs/draws 都是 `enabled && flag`，因此主门单独打开**不会**带上输入池；
  图像历史环由本地自包含路径 / Ctrl+Shift+C 单独 arm（`war3_frame_history.cpp` 的 `FrameHistoryControl arm`）。
- **禁止**：因为主门打开就隐式启用图像历史环（`profile.imagePreFrames` 256/96 的 arm）或 GPU 输入池
  （`RecorderProfile.inputSlots` 576/224）。本轮实机配置必须是 `frameEvidence=1, rawInputs=0, selfContained=0`，
  并与 `effectiveConfiguration` 一起冻结进申请书。
- **必须冻结实际总内存与导出预算**：记录 arm 时的 `RecorderMemory`
  （`totalVirtual` / `availableVirtual` / `availableCommit` / `largestFreeRegion`——这是**进程 VA/commit**，不是 RAM/VRAM）、
  环 `storageBytes` 与对象级份额（§2.2：watchlist 64 KiB + 终态预算），以及导出侧 `RecorderDiskHeadroom`=6 GiB 前置。
  报告只能按这三个口径写，不得写成"内存占用 X MB"。
- **配置面变更的测试代价（必须一并改）**：把子门加进 `RecorderConfiguration` 后，
  `effectiveConfiguration` 的键集合会变，因此除 `test_frame_recorder_default_policy_static.py` 外，
  还必须同步修改 `AutoTest/analyze_frame_evidence.py`（其 `effectiveConfiguration` 键集合是硬约束，
  见 `set(config)=={...}` 断言）与 `AutoTest/test_analyze_frame_evidence.py` 的 `configured()` 夹具；
  **禁止**只改生产代码而让解析器/夹具保持旧键集。

**缺链判定（分析器门禁，2026-09-17 03:05 上级裁定）**：

- **前提**：§2.2 的 4096 / 512 是**写入配额**，**不能**保证共享环里已写入的旧证据在会话结束前不被淘汰（`Ring` 淘汰是既有行为）；因此"配额未超"**不得**推出"证据完整"。
- **要求**：`AutoTest/analyze_frame_evidence.py`（及对象级解析测试）**必须**能从导出文件判定"证据链是否缺失"，三种形态必须**可判定且分别可读**：
  1. **写入即丢**（未入环）：`droppedPerFrame` / `droppedPerSession` 非零；
  2. **写入后被淘汰**（入环又出环）：`ringEvictedAfterRecord` 非零（对齐既有 `Snapshot.evicted`）；
  3. **未观测到**（表满 / 探测上限）：`droppedTableFull` / `droppedProbeLimit` 非零。
- **输出口径**：上述任一非零 ⇒ 分析器必须在结论与该对象条目上写「**缺链**」，并列出该对象当轮「**未覆盖**」；**禁止**输出"证据完整""未观察到错误矩阵"一类结论（与 §1.5、§5.4 的"不得外推"红线一致）。
- **测试要求**：必须新增解析测试（夹具导出文件）：(a) 带上述非零计数的夹具 ⇒ 断言产出缺链标记；(b) 全零夹具 ⇒ 断言**不**误报缺链；(c) 计数存在但事件流被截断 ⇒ 断言判为缺链，而不是"零事件 = 无问题"。该测试并入 §6 Step 1 门禁，**未过不得进入 Step 2**。

### 2.5 "后随观察"（recovery ledger）——回答 §1.2 的 Q2/Q3 的最小机制

仅记录拒绝路径，无法回答"之后是否被接住"。因此需要一个**有界、可关联、带状态机与终态**的后续观察表。
**本章是 2026-09-17 上级裁定 3 的修订版**：旧稿"第一次命中后即移除"**已被上级驳回**
（~~旧文：命中被观察对象后发 follow-up 并移除条目~~），因为第一次替代成功可能只是短暂抖动，
删掉条目就再也无法回答"压力解除后是否恢复"。裁定给出的四个必答条件是：**身份、状态机、保留、容量**。

#### 2.5.1 身份（裁定 3 条件一）

观察表的键 = §2.1 的 `ObjectKey`（会话 + map/device epoch + 生命周期身份 + 四元组定位分量）。
**不得**退回"只按 `renderablePart` / `runtimeModelPtr` 两指针匹配"：指针复用会产生假命中，
而 `rawcode` 是**类型**、不足以区分单位。无法取得生命周期身份或 epoch 的样本仍可入表，
但必须带 `identityWeak`，且**只用于"路径到达 / 关系记录"观察，不得用于跨帧恢复验收**（§5.2 / §5.3）。

```text
struct ObjectFollowUpEntry {          // 1024 槽开放寻址，static thread_local，零分配
  // ---- 身份（裁定 3 条件一） ----
  uint64_t  sessionGeneration;        // 必需
  uint64_t  mapEpoch;                 // 必需
  uint64_t  deviceEpoch;              // 必需（取不到则 0 + epochUnknown）
  uint64_t  lifecycleIdentity;        // 可证明的发布身份；0 ⇒ identityWeak
  uintptr_t renderablePart;
  uintptr_t runtimeModelPtr;
  uint32_t  jHandle, rawcode;
  uint32_t  flags;                    // identityWeak | epochUnknown | digestUnavailable | ...
  // ---- 状态机（裁定 3 条件二） ----
  uint32_t  state;                    // 见 2.5.2
  uint64_t  firstRejectFrame;         // 当前帧号（不是 manifest 帧、不是 palette frameTag）
  uint32_t  rejectReasonMask;         // 观察窗口内出现过的 R*
  uint32_t  rejectCount;              // 有界饱和计数
  uint64_t  firstHitFrame;            // 首次被具名来源接住
  uint64_t  lastHitFrame;
  uint32_t  hitCount;
  uint32_t  sawSubmit, sawDraw;       // 0/1
  uint32_t  terminalKind;             // 见 2.5.3；0 = 未判定
  uint64_t  envelopeFrame;            // 生命周期上限（≤3600 帧）
  uint32_t  slotAtIndex;              // 供 §3.3 路线 B 关联（只作线索）
  uint32_t  reserved;
};                                    // ≈104 B/条；1024 槽 ≈ 104 KiB（含墓碑保持容量不缩）
```

> 说明：旧稿条目身份只有 `renderablePart / runtimeModelPtr / jHandle / rawcode` 四字段，属**弱身份**；
> 本修订把**会话 / 代际 / 生命周期身份**提升为条目必需字段，并把 `slotAtIndex` 明确降级为**线索**（§3.3）。

#### 2.5.2 状态机（裁定 3 条件二）

每对象键的状态推进是**单向**的；阶段事件只在**首次**进入该阶段时发一条（阶段配额见 §2.2）：

> **2026-09-17 编号同步（实施后）**：阶段枚举在实现头文件（`war3_palette_object_evidence.h`）中冻结为
> `Rejected=1 / ServedCandidate=2 / Enqueued=3 / Drawn=4`；解析器（`AutoTest/analyze_palette_object_evidence.py`）
> 按**实现编号**冻结。下表左列为实现的枚举名，括号内为工单原稿用词（语义相同，仅改名以便与实现一致）。

| 状态 | 进入条件 | 发出的阶段事件 | 语义上限 |
| --- | --- | --- | --- |
| `Rejected` | A 链拒绝（`R0..R3`） | R 事件 | 只证明"该对象本帧走了拒绝路径" |
| `ServedCandidate`（原名 `FallbackAvailable`，`=2`） | 两条替代来源之一**可用**（`ProducerSnapshot` / `PoseKernel` / `ArenaSlot`） | follow-up 事件（`hitKind` + `hitKey`） | **只推进状态；绝不等于阴影恢复**（§5.2） |
| `Enqueued`（原名 `Submitted`，`=3`） | 同一对象键在后续帧出现**CPU caster 候选入队**证据（`shadowCasters.emplace_back`） | 提交事件（具名来源） | 只证明"进入 CPU 候选列表"；**不是 GPU 提交完成**，也不证明"画出来了" |
| `Drawn` | 同键出现**实际绘制**证据，或给出**具名剔除原因** | 绘制 / 剔除事件 | 证明"进入绘制"或"可证被剔除" |
| `Closed` | 满足 §5.2 闭合或触发 2.5.3 的任一中止态 | `closed` 事件（带 `terminalKind`） | 终态，之后才允许删条目 |

- **第一次 `FallbackAvailable` 只推进状态，绝不移除条目**（裁定 3 的核心）。
- 压力解除后的恢复判定见 §5.2：**不要求回到 `ArenaSlot`**；沿合法替代路径持续正确绘制**同样可能是正常结果**。
- 只有终态才允许删条目，且删除前**必须**先发 `closed` 事件。

#### 2.5.3 终态（裁定 K2：必须是不同终态）

下列终态**不得互相冒充**，全部写入 `bits[18]` 的 `terminalKind`，并在报告中分别列出：

> **2026-09-17 编号同步（实施后，必须以本节为准）**：终态枚举在实现头文件中冻结为
> `None=0 / Recovered=1 / WindowExpired=2 / ObjectGone=3 / TableFull=4 / EventLost=5 / Unclosed=6`；
> 解析器按**实现编号**冻结。下表已按实现编号排列（原稿的 0=Unclosed / 2=ObjectGone / 3=WindowExpired /
> 6=EvidenceInsufficient 序号**作废**，仅保留其语义：第 6 项语义并入 `Unclosed`，并新增 `None=0` 表示"非终态"）。
> 拒绝原因枚举同样以实现为准：`R0..R3` + `NotChecked(0xFE，该检查未执行)` + `Unknown(0xFF)`；
> 工单原稿的 `R0..R7` 未实现，读方不得按 R4..R7 解析。

| terminalKind | 名称 | 判定条件 | 证据不足 / 缺失时的处理 |
| --- | --- | --- | --- |
| 0 | `None` | **不是终态**（阶段事件的占位值） | 解析器必须拒绝把它当终态 |
| 1 | `Recovered` | 四段闭合：拒绝 + 替代命中 + **候选入队** + **绘制命令已记录**（实现要求 `sawSubmit && sawDraw`） | 仅同键、同 epoch 才算 |
| 2 | `WindowExpired`（超时） | 窗口结束且**从未被任何替代来源接住**（`hitCount==0`） | 发 `expired`；**不等同于** `ObjectGone` |
| 3 | `ObjectGone` | 有**可证依据**证明该对象已从 manifest / 场景消失 | 无依据不得判 `ObjectGone` |
| 4 | `TableFull`（容量耗尽） | 插入时表满且不做淘汰抖动 | 记 `droppedTableFull`；不得把该对象写成"消失" |
| 5 | `EventLost`（事件丢失） | 事件因每帧 / 每会话 / 终态预算或环淘汰被丢 | 记丢失计数器（含 `droppedTerminalReserve`）；该对象当轮**未覆盖** |
| 6 | `Unclosed` | 窗口结束仍未闭合（含原稿 `EvidenceInsufficient` 语义：关键字段当场不可得） | 明写"未闭合"——**既不是成功，也不是确认故障** |

#### 2.5.4 保留与容量（裁定 3 条件三、四）

1. **拒绝时**：查表；命中则更新 `rejectReasonMask / rejectCount`；未命中则插入
   （表满 → `droppedTableFull++` 并跳过，**不做淘汰抖动**）；同时发一条 R 事件（受 §2.2 上限约束）。
2. **接住 / 提交 / 绘制时**：只有当**同一所有者（同一记录线程）**观察到命中被观察对象时才发阶段事件。
   旧稿"全局 `g_paletteObjectWatchCount != 0` 才探测"已收窄：~~"表空即零成本"~~ 这一表述
   **不能当作零成本结论**（裁定 K3）——TLS 表 + 全局 watch count 在门开时仍有探测、缓存未命中与分支代价。
   **必须同时测量关闭与开启成本**（§2.2 探测上限、§6 Step 1 门禁）；并优先使用**同一所有者的状态**
   （本线程自己的表 + 线程局部计数），避免跨线程读取全局表。
3. **会话 / 地图切换**：`g_paletteSlotCacheSessionGeneration` 变化时清空整表
   （复用 `FindOrUpdatePaletteSlotCache` 顶部既有 TLS 会话检查点）；旧会话条目**绝不允许**冒充新会话。
4. **删除**：只允许在终态后删除；开放寻址的删除**必须写墓碑**，不得把槽位置空而截断碰撞链（裁定 K3）。
5. **与 §2.2 总额的关系（§R.5 A-1）**：终态 512 条是**从 4096 总计上限中预留**的份额，**不是**在 4096 之外另加。终态事件的写入同样占用 4096；环接近饱和时按 §2.2 先丢拒绝、后丢终态；任何因预算或淘汰而未写入 / 被淘汰的终态都必须体现在 §2.2 的丢失计数器中，并由分析器判定为**缺链**（§2.4）。

这条机制是本工单与 `2026-09-17-gapb-resource-key-fix-workorder.md`（P0 纯日志 trace）、
`2026-09-17-active-path-palette-instrumentation-workorder.md`（纯计数）的**主要区别**：
后两者都无法逐对象关联"拒绝 → 替代来源 → 是否提交 / 绘制 → 是否恢复"。

### 2.6 明确不做

- 不记录矩阵字节内容（§7-4 裁定 4：本轮**不批准新增矩阵扫描哈希**，也不记录矩阵字节内容）。
- 不对每个成功供出都发事件（会退化为全量取证）。
- 不在热路径写日志、不做文件 IO、不做跨线程查询、不分配。
- 不修改 `IsContractUnitCandidate`、`IsVisibleDirectGeosetUnitCandidate` 或任何准入谓词。
- 不把 device 侧（Gap A）与 shadow-core 侧（Gap B）的事件混入同一分母。
- 不新增 `Kind = 19`、不改既有 wire 布局（裁定 2）。
- 不为取证新增矩阵扫描哈希（裁定 4）。
- 不新增 JAPI / 玩家 API / 导出系统（裁定 7）。
- 不改槽位所有权准入（路线 A），也不用区间重叠宣布错误消费（裁定 5）。
- 不用 1902×963 的结构观测代替 2560×1440 性能/视觉基线（裁定 8）。

---

## 3. 槽位所有权证明设计

### 3.1 缺口形式化（上级 Q-C ① 与 A5 待办）

A0-A5 全链（`war3_shadow_renderer_core.cpp` → `FindOrUpdatePaletteSlotCache` 的
`else` 分支，收束变量 `producerConfirmed`）：

```text
A0 bindingHit              QueryRenderablePartPaletteSlot(part, slot, &groupCount, &frameTag)
A1 slotDomainValid         slot != 0xFFFFFFFF && slot < 0x3A98 && requiredCount != 0
                           && requiredCount <= 0x3A98 && slot <= 0x3A98 - requiredCount
A2 boundSlotIndexMatchesRemembered   slot == s_paletteSlotCache[i].paletteSlotIndex
A3 groupCountSuffices      boundGroupCount >= requiredPaletteCount
A4 bindingFrameFresh       currentPaletteFrameTag 可读且 != 0；boundFrameTag != 0；
                           boundFrameTag <= current；current - boundFrameTag <= kDelta
A5 slotRangeFrameFresh     QueryBlendedPaletteFrameTagRange(slot, requiredCount, min, max, missing)
                           && missing == 0 && min == max && max == boundFrameTag
                           && current - max <= kDelta
```

- `kDelta = kPaletteSlotCacheMaxFrameTagDelta = 0u`（严格同帧）。
- A5 的 `max == boundFrameTag` 只能证明**这些槽位的字节是"本帧写的"**，
  **不能证明"写这些槽位的人是 `renderablePart` 的 producer"**。
- 缺口触发形态（上级原文）：**FROZEN 携带 + 槽位被重分配给别的 part**。
  代码依据：`war3_model_hook.cpp` → `CaptureRuntimeGroupPaletteBindings` 在
  `slotIndex == 0xFFFFFFFF || slotIndex >= 0x3A98u` 时**沿用 bindings 表上次记录的 slot**
  （注释自述 "FROZEN 段里 War3 引擎的 8-帧 slot cadence"），
  随后仍以**本帧的 frameTag** 调用 `RecordRenderablePartPaletteBinding`。
  于是绑定 frameTag 是新的；只要 arena 的同一槽位本帧被**别的 part** 重写，
  A3/A4/A5 全部满足 ⇒ 供出的字节属于别的对象。
- 关键自述佐证：同处注释写的是"**通常**属于同一个 CModel，它的逻辑 slot 位置**不会**在
  arena 里迁移"——这是一个**假设**，不是证明。
- 既有 `render::skin::Selection` 契约明确：`slotAllocationGeneration` 
  "stays zero until an actual native allocator witness exists.
  Zero never authorizes rereading the global arena."
  ⇒ **自证的槽位标签不能命名为"所有权"**，只能命名为"最后已知 owner"。
>
> **2026-09-17 上级裁定（K5）**：所有权错误**可以在 A5 全通过时发生**
> （FROZEN 携带 + 槽位被重分配给别的 part：A0-A5 全满足 ⇒ 供出的字节属于别的对象）。
> 因此**"先出现 R3 `SlotRangeStale` 才允许调查所有权"的前置已被删除**（§6 Step 3）；
> 所有权调查不依赖任何拒绝计数出现。反过来，`RejectedStale` 也**不能**被解释为
> "确认发现陈旧矩阵"（§3.5）。

### 3.2 路线 A：per-slot owner/part 标签（改语义，需独立候选）

> **2026-09-17 上级裁定 5：本轮不批准路线 A 改准入。** 本节仅作为**可追溯备选**保留；
> 路线 A 会改变 caster 准入结果，须另立候选 + 独立实机反例门，且不得与 §3.3 的 B 合并验收。

**做法**：在 `s_slotBlendedPaletteCache` 的条目里增加 owner 见证，
并把 A5 升级为"该区间所有槽位的 owner 见证 == 本对象"。

- 落点：`war3_model_hook.cpp` → `struct BlendedPaletteEntry`（当前
  `Matrix4 matrix; uint32_t frameTag; uint64_t writeSerial; bool valid;`）。
  增加 `std::atomic<uintptr_t> lastOwnerPart` 与 `std::atomic<uint32_t> lastOwnerFrameTag`
  会令 65536 槽的数组增加约 65536 × 12 B ≈ **768 KB**（相对既有 ≈4.6 MB 约 +16%）。
- **谁来盖戳**——这是路线 A 最难的一步：
  - `CaptureBlendedPaletteSlotRange` 有**两个**调用方：
    - `CaptureRuntimeGroupPaletteBindings`（**知道** `partPtr`，正读该 part 的 arena 区间）；
    - `Hook_RuntimeMatrixWrite`（**不知道** `renderablePart`，只有 `nodePtr`（CGeosetData）
      与 `destMatrixPtr` → slot）。
  - 若只有一侧盖戳，另一侧写入必须**清除**戳记；否则戳记会陈旧并冒充所有权。
  - 从 `nodePtr`（CGeosetData）反推 `renderablePart` 的映射 **待验证**（需要新的
    一手逆向证据）；没有它，writer hook 侧无法盖出可信戳记。
- **风险**：这引入新的"所有权"判定，会改变 caster 准入结果（可能误杀合法帧）。
  按纪律必须作为**独立候选**，且需实机反例门；不得与本工单 Step 1 合并。
- **可验证方式**：
  1. 静态测试：钉死两个调用点的盖戳/清除路径与常量单源；
  2. 新计数 `g_paletteSlotOwnerStampMismatchCount`：每次 A5 通过时比较
     "戳记 owner" 与 "消费 part"，不等则 +1；
  3. 接受标准：实机 N 帧该计数 == 0（这是**反例被否证的观察**，仍不等于所有权已证明）。

### 3.3 路线 B：帧内 trace 观察（不改语义，**本轮已获批准的有界观察**）

**裁定 5**：上级**先批准 B 的有界观察，不批准 A 改准入**。B 只能记录"重写 / 读取关系"，
**不能凭区间重叠宣布错误矩阵被消费**（旧稿"证伪"措辞已被收窄，~~旧文见 §R.3~~）。

**做法**：不动所有权语义，只在帧内记录"谁写了哪些槽位、何时写、读的是哪一带"，供离线分析建立**事件顺序与来源域**。

- **两类"写"必须分开（裁定 K4）**：
  - `BindingRead`：读 native arena 后**更新插件缓存**（`CaptureRuntimeGroupPaletteBindings` 侧，
    知道 `partPtr`，读的是该 part 的 arena 区间）；
  - 游戏侧写入：`Hook_RuntimeMatrixWrite`（`0x12E600` 一族）**重新写 arena / 槽位矩阵**
    （只有 `nodePtr`（CGeosetData）与 `destMatrixPtr` → slot）。
  - `BindingRead` **不是**"游戏重新写了 arena"；把两者混为一类会伪造"重写"证据。
- 写入侧记录（有界）：在 `CaptureBlendedPaletteSlotRange` 内记录
  `(startSlotIndex, count, frameTag, writerIdentity, writerThreadId, writerSource, orderIndex)`。
  - `writerSource ∈ { WriterHook(0x12E600), BindingRead }`（**必须分列，不得合并**）；
  - `writerIdentity`：writer hook 侧只能给 `nodePtr`（CGeosetData）；binding 侧给 `renderablePart`；
  - `orderIndex`：同帧内的**事件顺序**（记录点自增序号）；
  - **可用 write serial**：沿用条目已有的 `writeSerial`（或等价既有序号）；没有就记 0 + unknown，**不得**为取证新增序号。
- 读取侧记录：A5 求值点记录 `(renderablePart, boundSlotIndex, requiredCount, boundFrameTag, currentFrameSerial, nativePaletteFrameTag)`。
- **分析判据（收窄版）**：同一 `nativePaletteFrameTag` 内，是否存在**同一槽位区间被 ≥2 个不同来源域 / 不同 writerIdentity 写入**，
  且某条 A5 通过样本的 `[slot, slot + count)` 与该区间相交。满足时最多记一条**调查线索**（`writing-overlap-suspect`）。
- **B 的硬限制（必须写进报告，裁定 5 / K4）**：
  - `nodePtr` 与 `renderablePart` **属于不同身份域，不可直接对拍**；
  - "同帧两次写且区间相交"**只是调查线索，不是错误消费证明**；
  - 即使相交，也要先分别证明"缓存标签 / 实际读取的 arena 字节 / 对象所有权"（§5.6）；
  - 要回答"那次写入属于哪个 part"需要 §3.2 的映射（**待验证**），本轮不为此改准入。
- **有界性**：每帧固定数组 256 条 × ≈40 B = **10 KiB `thread_local`**；溢出即计数并**停止本帧记录**（不是丢单条）；受 §2.2 丢失计数约束。
- **改动面**：仅 `CaptureBlendedPaletteSlotRange` 与 A5 站点；**零准入改动**；env 默认关。
- **可验证方式**：输出两个计数——出现多来源写入槽位区间的帧数、与 A5 通过样本相交的次数。
  在复现地图上若长期为 0，属"**未观察到**"（证据，不是证明）。

### 3.4 比较与建议

| 维度 | 路线 A（owner 标签） | 路线 B（帧内 trace 观察） |
| --- | --- | --- |
| **上级裁定 5** | **不批准改准入**（本轮） | **批准有界观察**（本轮先做） |
| 改动面 | `BlendedPaletteEntry` + 两个 writer 站点 + A5 判定 | 一个写函数 + A5 站点 |
| 内存 | +≈768 KB 常驻 | +10 KiB `thread_local` |
| 准入风险 | **会改判定**（可能误杀） | **零准入改动** |
| 前置依赖 | CGeosetData→part 映射（**待验证**） | 无（但因此无法定位 owner） |
| 能证明什么 | 若戳记可信，可用于所有权判定 | 只能记录"重写 / 读取关系"与事件顺序；**不能宣布错误矩阵被消费** |
| 结论 | **未获批准**，只能作为独立候选另案提交 | **本轮先做**；重叠相交最多是**调查线索** |

**建议（按裁定 5 修订）**：先做路线 B 的有界观察；即便 B 观察到"帧内多来源 / 多 writer 重写"，
也**不得**据此宣布"错误矩阵被消费"，只能作为调查线索，并回到 §5.6 的三层证明（缓存标签 / arena 字节 / 所有权）。
路线 A 未获批准，两者不得混验收。

### 3.5 报告口径（硬约束）

在**不改动所有权语义**之前（即路线 B 阶段及此前），报告：
- 只能写"**帧内新鲜度已收紧**"；
- **不得**写"槽位所有权已证明""错误矩阵不再被使用""arena 读已排除陈旧字节"；
- ①项措辞必须沿用上级 Q-E 的规定：
  "**已观察到记忆槽位复核拒绝路径；'错误矩阵不再被使用'尚未证明**"；
- 必须显式声明：`RejectedStale` 含绑定缺失/数量不足等拒绝，不能全部解释为"确认发现陈旧矩阵"。

**统一口径（裁定 10，取代"只沿用旧 §8①"）**：本工单的证明表述一律按 **§5.6** 的五层分别书写——
「缓存标签 / 实际读取的 arena 字节 / 对象所有权 / 提交 / 绘制」各自证明到哪一步；
**不得**用一句"按 §8① 措辞"概括全部，也**不得**把 B 的调查线索写成"错误矩阵被消费"。

---

## 4. 线程关系证明要求

### 4.1 为什么必须先证明

A5 读的是**非原子**的 `s_slotBlendedPaletteCache`：

- 声明（`war3_model_hook.cpp`）：
  `static std::array<BlendedPaletteEntry, kSlotBlendedPaletteCacheSize> s_slotBlendedPaletteCache`；
  `BlendedPaletteEntry` 的 `Matrix4 matrix`（非原子）、`uint32_t frameTag`、`bool valid`。
- 读（`QueryBlendedPaletteFrameTagRange`）：
  `const auto& entry = s_slotBlendedPaletteCache[slotIndex + i];` 后直接读
  `entry.valid` / `entry.frameTag` —— **无锁、无 seqlock、无 atomic**。
  同类无锁读还出现在 `QueryBlendedPaletteBySlotIndexBestEffort` 等。
- 写（`CaptureBlendedPaletteSlotRange`）：同样直接赋值 `entry.frameTag / entry.writeSerial / entry.valid / entry.matrix`。

若 shadow-core 构建线程与上述写入线程不是同一线程，A5 就是 **C++ 数据竞争（UB）**，
可能读到"新 frameTag + 旧 matrix"的撕裂组合 —— 那么 A5 得出的"帧同源"结论
本身就建立在未定义行为之上，**不能作为证据**。
因此：**在 A5 的结论被当作证据之前，必须先证明两线程关系**。

### 4.2 为什么不能靠架构推断

`war3_shadow_runtime_bridge.cpp` 里有一条明确自述：

> "control-plane 不能同步替 render thread 消费 semantic build；否则 pipe 请求会把
> `buildFrameChunk` 压到控制线程上，低压图 tail 状态下很容易复现 3s 响应超时。
> 真正的消费必须发生在 scene submit / EndFrame 的 render-thread 小步推进里。"

这说明构建**落点**是运行时属性，不是编译期保证。
（对照 `war3_shadow_renderer_core.cpp` 内 `buildFrameChunk` 推进处注释：
"Keep semantic preview builds genuinely incremental on the render thread."）
既有的线程见证模式也存在（`war3_shadow_runtime_bridge.cpp` 的
`capture.ownerThreadId != uint64_t(::GetCurrentThreadId())` 一类断言），
但 **shadow-core 的 palette 路径目前没有任何线程见证**（该目录内检索不到 ownerThread 字段）。

**并且这不是理论风险——已有一条真实存在的 off-thread 消费路径**（本文于 2026-09-17 独立复核了下列锚点）：

- `war3/tools/war3_control_plane.cpp` 的 `DrainSemanticBuildFromControlPlaneIfAllowed`
  在 `CanDrainSemanticBuildFromControlPlane` 通过后调用
  `validationRuntime.drainPendingBuildForControlPlane(maxChunks, maxBudgetUs, …)`；
  而 `drainPendingBuildForControlPlane` 的实现位于
  `war3/shadow/war3_shadow_renderer_core.cpp` → `void ShadowValidationRuntime::drainPendingBuildForControlPlane(`，
  它会推进 `buildFrameChunk` ⇒ **palette A 链在调用者线程（控制面命名管道的分离线程）上执行**。
- 门控键为 `payload.value("allowControlPlaneSemanticDrain", false)`
  （`war3/tools/war3_control_plane.cpp` → `return payload.value("allowControlPlaneSemanticDrain", false) ||`）。
- `AutoTest/war3_autotest_mcp.py` 在多处 `get_shadow_runtime_summary` 请求里显式带上
  `"allowControlPlaneSemanticDrain": True` 与 `"semanticBuildDrainMaxChunks": 32`
  ⇒ **标准 AutoTest 工具链会走 off-thread drain**。
- 与既有纪律冲突：该项目文件 `war3/tools/war3_control_plane.cpp` 的既有认知是
  "control-plane 不能同步替 render thread 消费 semantic build"，
  但 drain 分支正是这样做的（其自身注释解释了为何**默认**路径只发请求）。

详细线程清单、危害分析与三选一修复方案见**同一目录的独立文档**
`docs/plan/2026-09-17-p0-thread-relationship-proof.md`（该文由另一 agent 先行产出，
本文不重复其推导，只把它作为 §4 的强制前置）。

### 4.3 证明方式与记录位置

要求：**记录并比较原始线程 id，而不是断言相等**。

1. **写入侧见证**（producer）：
   在 `BlendedPaletteEntry`（或 `RenderablePartPaletteBindingEntry`）增加
   `std::atomic<uint32_t> writerThreadId`，在 `CaptureBlendedPaletteSlotRange` 内
   `store(::GetCurrentThreadId())`（两个调用点都覆盖）。
   **待验证**：是否需要"首个 writer 胜出"语义以避免抖动（同一槽位多线程往返写会让
   "最后 writer"失去信息量）；建议同时记录一个 `writerThreadIdMask`（位集合）更稳。
2. **消费侧见证**（consumer）：
   对象级事件由 `evidence::Record` 自动戳 `event.thread = GetCurrentThreadId()`；
   另在 `bits[12]` 显式再记一份，便于离线对拍（不依赖导出实现细节）。
3. **与主循环线程的第二见证**：
   `hooks/war3_hook_lifecycle.h` → `DWORD GetMainLoopThreadId()` 已存在。
   同时在 `bits[13]` 记录 `GetMainLoopThreadId()`，
   使报告能区分"同线程"与"两个都不是主循环线程"两种情形。
4. **专用计数**：`g_paletteSlotCacheThreadAffinityMismatchCount`（本地原子），
   在 A5 通过点比较 `writerThreadId` 与 `GetCurrentThreadId()`，不等即 +1。
   导出沿既有链（bridge summary → diagnostics hub → control plane JSON → perf monitor JSON），
   与 `QueryPaletteSlotCacheSlotRangeStaleRejectCount` 同模式。
   **注意**：本计数与 `RejectedStale` **不同分母**，不得并入（上级 Q-C ③）。
5. **接受标准**：整轮实机 `mismatch == 0`，且观测到的 writer 线程 id 集合 ⊆ {消费线程 id}，
   报告必须列出**原始 id 列表**（防止将来出现第三个线程却无人察觉）。
6. **口径**：即便 mismatch 为 0，结论只能是"**已观察到同线程**"，
   不得写成"已建立内存序/已消除竞争"——代码里两者之间仍无内存序契约。
7. **必须标注消费线程类别**：每条对象级事件除 `bits[12] = GetCurrentThreadId()` 外，
   还要记 `bits[14]` 的一位 `consumedByRenderThread`（是否等于 `GetMainLoopThreadId()`），
   使报告能区分"渲染线程消费"与"控制面 drain 线程消费"。
   在 `allowControlPlaneSemanticDrain` 生效的运行里，**A5 相关结论一律不得采纳为证据**
   （见 `docs/plan/2026-09-17-p0-thread-relationship-proof.md` §6-C）。

### 4.4 若证明失败（mismatch ≠ 0，或消费发生在控制面 drain 线程）

- A5 相关的"帧同源"结论**立即标记为不成立**（fail-visible）。
- 此时**唯一可采纳**的证据是本工单 §2 的对象级记录（它记录原始观测值，不依赖 A5 的判定）。
- 需另立候选做两件事之一（本工单**不做**，也不在本文授权范围内；
  候选形态见 `docs/plan/2026-09-17-p0-thread-relationship-proof.md` §6 的 A/B）：
  - **A**：给逐槽读加与快照同源的 `TryCell`/原子守卫（改变拒绝分布，需重新解释实机细分计数）；
  - **B**：在 drain 入口拒绝非渲染线程消费（不改渲染/阴影语义，该文建议项）。
- 无论走哪条，实机报告必须**分别声明**"构建消费线程"与"槽位所有权"两项是否已证明。

### 4.5 已落地的前置与仍需逐轮核对的部分（只读核对，2026-09-17 修订 R2）

- 本树工作区已存在上一轮线程修复的**第一部分**：`ensureLatestFrameBuilt()` 在
  `requestLatestFrameBuild()` 之后、任何加锁 / 共享状态读取 / 推进调用之前加入线程门，
  非主循环线程先自增 `g_semanticBuildOffThreadRefusedCount` 再 return（`war3_shadow_renderer_core.cpp`）。
  这意味着**本轮实机可能不再出现管道线程 drain 消费**。
- **但这不改变本工单的规则**：修复 2（安全进度快照发布）与修复 3（具名拒绝原因）本轮未做，
  进度字段竞争仍在；报告仍必须**逐轮记录观察到的 writer / 消费线程原始 id 列表**，
  不得用"线程门已存在"外推为"竞争已消除"。措辞上限仍是"**已观察到主循环线程唯一推进**"。
- 4.3 第 5 条的接受标准（`mismatch == 0` 且 writer id 集合 ⊆ {消费线程 id}）不变；
  在 `allowControlPlaneSemanticDrain` 生效的运行里，A5 相关结论**一律不得采纳为证据**（4.3 第 7 条）。

---

## 5. 验收口径

### 5.1 总则

"对象级证据闭合" = 对**同一个对象键**（§2.1 的 `ObjectKey`：会话 + map/device epoch + 生命周期身份 + 四元组定位分量），
在**同一轮**、**同一 `sessionGeneration` / mapEpoch / deviceEpoch`** 内，形成可对拍的事件链。
按对象键逐一闭合，**不允许**用跨对象聚合替代。
每一轮的 evidence 会话、DLL 哈希、分辨率、轮次必须一并冻结。

- ~~旧表述：对象键 = 四元组~~ **已被上级驳回**（§2.1 / §R.3）。
- **弱身份样本（`identityWeak`）不得用于跨帧恢复验收**，只能支持"路径到达 / 关系记录"。
- 每条事件必须能分辨三个帧（当前帧 / manifest 帧 / native palette frameTag，§2.1）；
  任何"取其中一个填 `frame`"的写法都已被驳回。

### 5.2 正例（合法对象仍被正确绘制）需要什么

对同一对象键，需在**同段、同 epoch** 内区分四步；**每一步只证明它自己**：

1. **拒绝**：一条 A 链拒绝事件（`bits[0] ∈ {R0, R1, R2, R3}`），带实际键
   （`requiredPaletteCount / currentSlotIndexRaw / rememberedSlotIndex / boundSlotIndex`）。
2. **替代 palette 可用**：一条 follow-up 事件，`bits[0]` 为**具名来源**
   （`ProducerSnapshot` / `PoseKernel` / `ArenaSlot`），带 `hitKind` + `hitKey` + 帧差。
   **这一步只推进状态，不得判为"恢复"**（裁定 3 / K2）。
3. **实际提交关联**（必需）：同一对象键在**后续帧**出现"该 packet 被提交"的证据
   （例如 shadow-core 提交计数与对象键的对应事件，或 canonical / manifest 侧同键提交标记）。
   **只拿到 palette 不算恢复。**
4. **实际绘制关联**（必需）：该提交确实进入绘制（shadow map 对该 caster 的绘制计数 / 对应事件），
   **或**给出该对象被剔除的**具名**依据。
5. **压力解除后的状态**：同一对象键在**段 3**（压力解除段）继续满足第 3、4 条——
   **不要求回到 `ArenaSlot`**：沿任一**合法替代路径**（`ProducerSnapshot` / `PoseKernel`）
   持续正确绘制，**同样可能是正常结果**；或给出该对象已从 manifest 消失的**可证依据**。

闭合判定与终态：

- 只有第 1+2 条 ⇒ 记「**观察结果（部分）**」，**不得判为恢复**。
- 只有 1+2+3 ⇒ 记「已接住并提交（未确认绘制）」。
- 1+2+3+4（且第 5 条成立或给出可证的消失依据）⇒ `terminalKind = 1 Recovered`。
- **证据不足 ⇒ `Unclosed`（未闭合）**：既不是成功，也不是确认故障。
- 超时（`WindowExpired`）、对象消失（`ObjectGone`）、容量耗尽（`TableFull`）、
  事件丢失（`EventLost`）是**四个不同终态**，不得互相冒充（§2.5.3）。
- 五条的 `sessionGeneration` / `mapEpoch` / `deviceEpoch` 必须相等；跨地图的事件链**不算闭合**。
- 上级原话保留：~~"工单把「重新拿到 palette」当成了「阴影恢复」"~~ —— 本轮已按此把提交与绘制列为必需。

### 5.3 反例（确实用了错误矩阵）需要什么

必须**实际观测到**，不是"存在可能性"；且必须先满足 §5.6 的分层表述。

- **本轮状态：反例门未闭合，也未获准用 B 的区间重叠来宣布。** 路线 A 未获批准（裁定 5）。
- 路线 B 口径（**已收窄为调查线索**）：同一 `nativePaletteFrameTag` 内某槽位区间出现 ≥2 个不同
  **来源域 / writerIdentity** 的写入，且某条 A5 通过样本的 `[slot, slot+count)` 与该区间相交。
  满足时最多报"**调查线索 `writing-overlap-suspect`**"，
  **不得**写成"错误矩阵被消费""错误矩阵已证明被使用"。
- 要把线索升级为结论，至少还要分别闭合 §5.6 的第 2、3 层（实际读取的 arena 字节属于谁 / 对象所有权）；
  本轮**没有**授权用重叠代替这两层。
- 单独"某一帧某槽位被写两次"**不构成**任何超出"线索"的结论。
- `BindingRead` 与"游戏重新写 arena"**必须分列**（§3.3 K4）；把前者算作重写即为伪造反例。
- **弱身份样本（`identityWeak`）不得单独作为反例或正例**（§2.1 / §2.5.1）。
- ~~旧表述：路线 A 口径 `g_paletteSlotOwnerStampMismatchCount > 0` 即可~~ —— 路线 A 本轮未获批准。

### 5.4 不得用什么冒充（口径红线）

| 冒充物 | 为什么不算 |
| --- | --- |
| 拒绝计数增长（31,881→88,926→91,039 一类） | 只证明路径到达；不含对象身份，无法回答 §1.2 三问 |
| `ServedAfterConfirm == 0` | 只支持"该运行未观察到"；不得定性为死代码 |
| `ProducerSnapshotFallback` 增长 | 是分母，不是同一对象被接住的证明 |
| 细分拒绝之和 == 聚合 `RejectedStale` | 只证明分母自洽 |
| "接线完成"/"字段已导出" | 上级 Q-B：这四项只证明路径到达与提交数量 |
| 单个对象样本外推全体 | 对象级证据是逐对象闭合，不做外推 |
| 受控终止下驱动 exit 0 | 上级 Q-E：`forced=true` 的 exit 0 ≠ 游戏自然退出已验收 |
| `IsSemanticSceneSubmissionRuntimeEnabled` 类构建门打开后得出的结论 | 上级 Q-D：`ENDFRAME_BUILD`/`PUBLISH_REGISTRIES_BEFORE_SCENE` 不得外推为默认生产路径 |
| 用"合同 ON/OFF 差异"解释对象级证据差异 | 对象级证据门**不是**合同 env，不得与 M2 矩阵混淆 |
| 摘要相等（已算好的 hash） | 裁定 4：**摘要 ≠ 所有权证明**，只支持"同一份已发布拷贝" |
| 把三个"帧"混成 `frame` | 裁定 K1：当前帧 / manifest 帧 / native palette frameTag 必须分列 |
| 用 B 的区间重叠宣布错误消费 | 裁定 5：最多是**调查线索**；且不得越过 §5.6 第 2、3 层 |
| "表空即零成本" | 裁定 K3：TLS 表 + 全局 watch count 不构成零成本结论，必须测量关闭与开启成本 |
| 用 1902×963 结构观测结果谈性能 / 视觉 | 裁定 8：不能代替 2560×1440 性能/视觉基线 |

### 5.5 轮次与分辨率

- 对象级证据需要**多轮**（至少一轮复现 + 一轮对照），每轮独立会话与独立导出文件；
- 比例/最大值的报法沿用 P0 口径：0.5s 采样是"采样点最大值/占比"，不是完整逐帧统计；
- **分辨率（2026-09-17 上级裁定 8）**：**原则批准**本工单向上级申请**实际客户区 1902×963**
  （上一轮实测值；~~旧表述"本工单不预设任何分辨率"~~ 已被部分推翻）。
  - 该租约**仅限结构观测**（对象级事件是否存在、状态机与终态是否可判读、覆盖路径是否为 0）；
  - **具体租约**（轮数、段长、矩阵、恢复合同、DLL 冻结哈希）**待 Step 1 冻结后另行审批**；
  - **不得**用 1902×963 的性能或视觉结果代替 **2560×1440 性能/视觉基线**，也不得外推到 2560×1440；
  - 无法满足 2560×1440 时的替代分辨率申请仍按上级 Q-D：**先申请后跑，不得先跑后说明**。

### 5.6 证明分层口径（裁定 10：统一修订，取代"只沿用旧 §8①"）

下列五项必须**分别**写清"证明到哪一步"，任何一项都不得用另一项的结论顶替：

| 层 | 名称 | 本轮能证明到什么 | 明确**不能**证明 |
| --- | --- | --- | --- |
| ① | **缓存标签**（slot / binding / frameTag / 记忆槽位） | 记忆槽位复核路径**被到达并发生拒绝**（当轮计数 + 对象级事件的 key 字段）；A5 的 `min==max==boundFrameTag` 只说明"这些槽位本帧被写过" | 标签**自证**不等于所有权；同帧标签不能证明 arena 字节与所有者一致；`RejectedStale` 不能全部解释为"发现陈旧矩阵" |
| ② | **实际读取的 arena 字节** | 若对象键事件链完整，能说明**哪次读取被发起**、读到哪个 slot 区间、当场 frameTag 是多少 | 不能证明读到的字节**属于**该 part（这正是 §3.1 缺口）；本轮也**没有**内容哈希可比对（裁定 4） |
| ③ | **对象所有权** | 本轮**只能记录重写 / 读取关系与事件顺序**（B 的有界观察，裁定 5） | 不能宣布所有权已证明；不能凭区间重叠宣布错误矩阵被消费；`slotAllocationGeneration` 仍为 0（无 native allocator witness） |
| ④ | **提交** | 若能采到同键提交事件，能证明"该 packet 进入提交" | 不证明"画到了屏幕上"；也不证明"替代 palette 可用" |
| ⑤ | **绘制** | 若能采到同键绘制事件或**具名**剔除依据，能证明"进入绘制 / 可证被剔除" | 不证明视觉正确性；画面反例门仍未闭合（§5.4） |

- 报告模板必须逐层写"**已观察到 / 未覆盖 / 未闭合**"三态之一，**不得**跨层合并成一句结论。
- **旧 §8① 措辞只保留在 ① 层**（上级 Q-E 原话："已观察到记忆槽位复核拒绝路径；错误矩阵不再被使用尚未证明"），
  不得再作为全部五层的统一口径。

---

## 6. 实施顺序与成本

每步独立门禁、独立冻结；**任一步未过不得进入下一步**。

### Step 0 — 只读回溯（零代码，成本最低）

- 动作：先检查既有 P0 产物（`docs/plan/2026-09-17-p0-real-machine-execution-record.md`
  引用的报告与 `samples.jsonl`）里是否**已经**含有可逐对象关联的字段。
- 门禁：给出"可/不可回溯"的明确结论与依据位置。
- 预期（**待验证**）：不可回溯（现有导出只有聚合计数），因此本步的产出是"为什么必须新增记录"。
- **覆盖路径结论（K6，已由附录 D 支撑）**：旧产物里 shadow-core 路径 `resolved=0`，
  因此 Step 0 的结论必须同时写"**本工单要覆盖的路径在旧轮未被覆盖**"，不得写成"路径已证明健康"。
- 成本：纯只读，无构建、无实机。

### Step 1 — 有界对象级记录（默认关，零准入改动）

- 内容：§2.1 字段 + §2.2 预算 + §2.5 watchlist（含状态机 / 终态 / 墓碑）+ §2.3 复用 `evidence`
  （`palette-object/v1`）+ §2.4 导出（**子门进入 `RecorderConfiguration`**：`DXVK_WAR3_FRAME_EVIDENCE_PALETTE_OBJECT`，
  默认关、受主门约束、effective configuration 可见）。
- 门禁：
  - 新增静态测试（建议 `AutoTest/test_palette_object_evidence_static.py`）：字段映射、上限常量单源、
    fail-closed 分支、会话清理、阶段配额、墓碑删除不截断碰撞链、"成功路径仅在命中被观察对象时探测"；
  - **解析测试（裁定 2 的批准条件）**：钉死 label / kind / `bits[0]` 枚举域（R0..R7 + 终态）、
    `data` 维度、NUL 终止、未知枚举 fail-visible；
  - **缺链判定门禁（2026-09-17 03:05 上级裁定，硬门禁）**：分析器必须能从导出文件判定缺链——
    ① 写入即丢（`droppedPerFrame`/`droppedPerSession`）；② 写入后被共享环淘汰（`ringEvictedAfterRecord`）；
    ③ 表满 / 探测上限（`droppedTableFull`/`droppedProbeLimit`）。任一非零 ⇒ 输出「缺链」并把该对象记为
    「未覆盖」，**不得**输出"证据完整"；须有解析测试（非零夹具 ⇒ 缺链；全零夹具 ⇒ 不误报；事件流截断 ⇒ 判缺链）。
    **配额不能保证旧证据不被淘汰，本条未过不得进入 Step 2**（§2.2、§2.4、§R.5）。
  - 生产函数测试（仿 `AutoTest/test_frame_evidence_runtime.cpp` 与 `meson.build` 的
    `war3_frame_evidence_runtime_test` 目标）：watchlist 的容量 / 溢出 / 墓碑 / 清理 / 五个丢弃计数；
  - **配置面同步**：`AutoTest/analyze_frame_evidence.py` 的 `effectiveConfiguration` 键集合硬断言、
    `AutoTest/test_analyze_frame_evidence.py` 夹具、`test_frame_recorder_default_policy_static.py` 必须一并改；
  - 既有门禁必须仍过：`test_frame_recorder_memory_static.py`、`test_frame_evidence_control_static.py`、
    `test_palette_slot_cache_producer_confirmation_static.py`、`test_palette_slot_cache_counters_export_static.py`；
    **`AutoTest/test_recorder_event_wire_golden.py` 不应被改**（这正是复用 `Kind::ShadowState` 的原因，已获批准）；
  - **成本测量门禁（裁定 9 / K3）**：必须给出"子门关 / 开"的 CPU 成本量级
    （至少一次宿主侧微基准或等价可复现测量），以及"表空但门开"的探测成本；不得只声明"默认关所以零成本"；
  - `ninja -C build32` exit 0；`ninja -C build32 -n` no-work；`meson test -C build32` 全过；AutoTest 静态全量。
- 成本：源文件约 1 个（shadow-core）+ 1 个头（若需）+ 2-3 个测试 + `meson.build` 一个目标；
  运行时开销：门关 ⇒ 0（须测量证实）；门开 ⇒ 仅拒绝路径 + 命中被观察对象时的一次键探测（受 §2.2 上限）。
- **覆盖路径声明（K6）**：Step 1 落地后必须先用离线 / 宿主测试说明本记录覆盖的实际路径
  （§1.5 的 `FindOrUpdatePaletteSlotCache` else 分支 + `tryEngineDirectPosePalette`），
  并在实机申请书中写明"若 `resolved=0` 则本轮记为未覆盖"。
- 实机申请条件：DLL 冻结哈希 + §2.2 常量冻结 + **CPU-only 配置冻结（裁定 9）** +
  明确轮数 / 矩阵 / 分辨率（1902×963 结构观测租约）/ 恢复合同的申请书。**本轮不授权开跑**（上级 Q-D）。

### Step 2 — 线程关系见证（§4）

- 内容：writer 线程戳 + 消费线程记录 + `g_paletteSlotCacheThreadAffinityMismatchCount` + 导出。
- 门禁：静态测试钉死计数与导出链（同 `test_palette_slot_cache_counters_export_static.py` 模式）；
  构建/no-work/meson/AutoTest 全量。
- 成本：小（两个结构体字段 + 计数 + 导出字段）。
- 前置：**必须在任何 A5 结论被引用之前完成**。
  该前置在本树工作区已有**第一步落地**（§4.5：非主循环线程不再推进构建），
  但"已观察到同线程"仍必须逐轮记录原始线程 id，不得外推为竞争已消除。

### Step 3 — 路线 B 帧内 trace（§3.3）

- 前置（2026-09-17 上级裁定 K5 修订：**旧前置已删除**）：
  ~~必须先实测到存在"R3 `SlotRangeStale` 且无替代来源"的对象~~。
  所有权错误**可能在 A5 全通过时发生**，因此 B 的观察**不以出现 R3 为条件**；
  本步保留的工程前置是"先有 Step 1 的有界记录与丢失计数基础设施"。
- 门禁：trace 有界性测试（每帧 256 条、溢出停止并计数）、离线分析器
  （可仿 `AutoTest/analyze_frame_evidence.py` 单独脚本，不侵犯 `AutoTest/` 只读纪律）。
- 成本：中（写函数改造 + 分析器）。

### Step 4 — dev 候选（默认关，仅在上一步给出实测依据后）

- 可能形态：资源键补全（`2026-09-17-gapb-resource-key-fix-workorder.md` 的 P1 谓词）、
  ~~或路线 A 的槽位 owner 见证~~。
  **注（裁定 5）**：路线 A 的 owner 见证**本轮未获批准**，不得作为 Step 4 形态；
  Step 4 只允许"资源键补全"一类**已另行裁定**的修复，且必须默认关、独立候选、独立实机门。
- 硬约束：**必须默认关**；`dev` 门不是安全检查的替代品；
  **不得在同一次变更中放宽 `IsContractUnitCandidate`**。
- 实机申请条件：新冻结记录 + 明确轮数/矩阵/尺寸/恢复合同 + 上级裁定。

---

## 7. 上级裁定结果（原 10 项问题已逐条裁定）

> 本节保留原问题清单可追溯（被驳回的选项加 ~~删除线~~），并在每项下就地给出**已裁定**结论与落点。
> 上一轮 08:2x 的 Q-A..Q-E 裁定仍是本工单的上位约束；本轮十项是对下面 10 个问题的正式答复。

1. **子门形态**（原问题）：新增 `DXVK_WAR3_FRAME_EVIDENCE_PALETTE_OBJECT` 是放进
   `RecorderConfiguration`（会改 `effectiveConfiguration` 面与 `test_frame_recorder_default_policy_static.py`），
   ~~还是在 shadow-core 内独立读 env（不扩配置面但报告不可见）~~？
   **裁定 1：批准进入 `RecorderConfiguration`**——默认关、受主门约束、effective configuration 可见；
   **禁止**在 core 内另造隐藏 env 入口。落点 §2.4 / §2.3 / §6 Step 1。
   **追加代价（本文核实）**：`effectiveConfiguration` 键集合是 `AutoTest/analyze_frame_evidence.py` 的硬断言，
   因此还必须同步改该分析器与 `AutoTest/test_analyze_frame_evidence.py` 夹具。

2. **是否复用 `Kind::ShadowState` + label `palette-object/v1`**（本文建议，规避
   `test_recorder_event_wire_golden.py` 的 `kind 1..18` 硬校验），
   ~~还是要正式扩到 `Kind = 19`（同时改 wire 校验与 golden）~~？
   **裁定 2：批准复用**；须冻结字段映射、阶段/原因枚举及**解析测试**。落点 §2.1 / §2.3 / §6 Step 1 / §8.11。

3. **§2.5 的"后随观察表"是否属于已批准的"有界对象级记录"范围**？
   它在成功路径上引入了一次哈希探测（仅当有被观察对象时）——是否接受？
   **裁定 3：有条件批准**；条件是把**身份 / 状态机 / 保留 / 容量**四件事写清，且
   **不得第一次 fallback 后即删**。落点 §2.5（2.5.1-2.5.4）/ §5.2 / §5.3。
   注意：旧表述"表空即零成本"**不能**作为零成本结论（K3），须测量关闭与开启成本。

4. **是否允许记录字节内容哈希**（每事件约 12 KB 哈希计算）？本文默认**不做**。
   **裁定 4：本轮不批准新增矩阵扫描哈希**；可记录**已算好**的摘要及其布局/来源，
   且**摘要 ≠ 所有权证明**。落点 §2.1 / §2.3 / §2.6 / §5.4。

5. **槽位所有权路线**：先做路线 B（零语义）还是直接授权路线 A（改判定）？
   **裁定 5：先批准 B 的有界观察，不批准 A 改准入**；B 只能记录重写/读取关系，
   **不能凭区间重叠宣布错误矩阵被消费**；若走 A 需先确认 CGeosetData→renderablePart
   映射的一手依据由谁提供（**本轮无**）。落点 §3.2 / §3.3 / §3.4 / §5.3 / §6 Step 3-4。
   **并删除**"先出现 R3 才允许调查所有权"的前置（K5）。

6. **buffer/key 的 `owner` 字段语义**：~~`event.key.owner` 该填什么（`renderablePart` / 设备指针 / 0）？~~
   **裁定 6：`key.owner` 保持既有设备/记录上下文语义，不填 part 指针**（part 放载荷）；
   取不到 owner 记 0 并标 unknown，**不得据此强关联**。落点 §2.3。

7. **dump 出口**：pipe（`command == "frame_evidence"`）是否足够？
   ~~是否需要新增 JAPI `warvk:v1` 出口？~~
   **裁定 7：批准复用现有 pipe/export，不新增 JAPI**；不扩张玩家 API、不另造导出系统。
   落点 §2.4 / §5.4。

8. **实机分辨率**：上级 Q-D 要求替换分辨率须**先申请**。
   上一轮为 1902×963，本工单需明确本次申请的分辨率与轮数。
   **裁定 8：原则批准申请实际客户区 1902×963**，仅限**结构观测**；
   具体租约待冻结后审批；**不能代替 2560×1440 性能/视觉基线**。落点 §5.5 / §6 Step 1。

9. **主门选择**：复用 `DXVK_WAR3_FRAME_EVIDENCE=1` 会同时开启既有取证内存/磁盘预算。
   上级已撤回"纯 env trace 零风险"，本文据此**不声称零风险**并给出独立预算；请确认该取舍。
   **裁定 9：主门有条件批准 CPU-only 配置**；不得因主门开启而隐式启用图像历史环 / GPU 输入池；
   须冻结**实际总内存**与导出预算。落点 §2.4 / §2.2 / §6 Step 1。

10. **Gap B 三处证明口径的修正文案**（arena vs slot cache 同帧标签、
    `>=`→`==` 未新增排除证明、`BindingMiss`/`SnapshotFrameStale` 不同分母）
    ~~是否仍按 `docs/plan/2026-09-17-gapb-confirmation-strengthening-design.md` §8① 的现有措辞执行~~？
    **裁定 10：要求统一修订，不能只沿用旧 §8①**；必须**分别**写清
    「缓存标签 / 实际读取的 arena 字节 / 对象所有权 / 提交 / 绘制」各自证明到哪一步。
    落点 §5.6（新）/ §3.5。

---

## 8. 附录 A：一手位置索引

> 全部为 2026-09-17 在 B 树只读观测。**行号会漂移，请以"唯一子串/函数名"锚定。**

### 8.1 `src/d3d9/war3/shadow/war3_shadow_runtime_contract.cpp`

| 锚（唯一子串 / 函数名） | 行 | 证明什么 |
| --- | --- | --- |
| `bool IsContractUnitCandidate(const ShadowRenderableRecord& record)` | 450 | 未获批准的准入谓词本体：`ObjectKind::Unit && groupIdx <= 0 && unitPtr && (jHandle \|\| rawcode)` |
| `TryPublishMissingVisibleUnitGeosetBinding` | 548 | `IsContractUnitCandidate` 调用点 1 |
| `DemandFillVisibleUnitGeosetBindings` | 571 | `IsContractUnitCandidate` 调用点 2；`kMaxDemandFillPerCapture = 64u` 先例 |
| `uint32_t VisibleDirectGeosetUnitRejectReason` | 476 | 直读 geoset 路径的 7 档拒绝原因（对象级记录的既有形态先例） |

### 8.2 `src/d3d9/war3/shadow/war3_shadow_renderer_core.cpp`

| 锚 | 行 | 证明什么 |
| --- | --- | --- |
| `struct PaletteSlotCacheEntry {` | 752 | 记忆槽位缓存结构（仅 3 字段，无对象身份） |
| `static constexpr size_t kMaxPaletteSlotCacheEntries = 4096;` | 757 | 缓存容量；`thread_local` 数组 |
| `g_paletteSlotCacheBindingMissRejectCount` 声明 | 775 | R0 的聚合对应物（**不导出**） |
| `static constexpr uint32_t kPaletteSlotCacheMaxFrameTagDelta = 0u;` | 773 | 严格同帧容差；放宽上限为 2 |
| `static uint32_t FindOrUpdatePaletteSlotCache(` | 783 | A0-A5 全链所在函数 |
| `QueryRenderablePartPaletteSlot(` （A0） | 822 | 绑定查询调用点 |
| `const bool slotDomainValid =` （A1） | 827 | 溢出安全上界写法 |
| `const bool boundSlotIndexMatchesRemembered =` （A2） | 831 | 与记忆槽位一致 |
| `const bool groupCountSuffices =` （A3） | 834 | 数量覆盖 |
| `const bool bindingFrameFresh =` （A4） | 842 | 绑定帧不旧 |
| `const bool slotRangeFrameFresh =` （A5） | 850 | 逐槽区间帧同源（**缺口所在**） |
| `const bool producerConfirmed =` | 861 | 六项收束 |
| `g_paletteSlotCacheBindingMissRejectCount.fetch_add` | 877 | 细分拒绝 1/4 |
| `g_paletteSlotCacheSnapshotFrameStaleRejectCount.fetch_add` | 6692 | R4（已加 `currentReadable && snapshotFrameTag != 0u` 门） |
| `auto tryEngineDirectPosePalette = [&]() -> bool {` | 6651 | 来源选择链适配层（S2 收窄版） |
| `static constexpr uint32_t kProducerSnapshotMaxCount = 64u;` | 6665 | S0 上限 |
| `outPalette.size() == size_t(requiredCount)` | 6672 | S1（原 `>=`，**未新增排除证明**） |
| `RenderablePartFieldOffsets::StagePresetSpanBaseIndex` | 6705 | 查询键 `+0x08` 的真实读取点 |
| `paletteSlotIndex = FindOrUpdatePaletteSlotCache(` | 6711 | 调用点（转发 `requiredCount`） |
| `kGlobalPaletteBufferRva` | 6727 | Game.dll 全局 arena 解引用 |
| `outPalette[i] = DecodeRuntimePoseMatrix48(enginePalette + i * 48u);` | 6744 | 实际按 slot 键读 arena 字节 |
| `auto logFailure = [&]() {` | 6781 | 失败诊断面（S2 未改一行） |
| `uint64_t QueryPaletteSlotCacheSlotRangeStaleRejectCount()` | 7137 | R3 导出访问器 |
| `uint64_t QueryPaletteSlotCacheSnapshotFrameStaleRejectCount()` | 7141 | R4 导出访问器 |
| `ShadowResolveStats ShadowRendererCore::buildFrame(` | 7193 | 构建入口 |
| `size_t ShadowRendererCore::buildFrameChunk(` | 7210 | 分块构建（render thread 小步推进） |
| `bool ShadowRendererCore::resolveRecord(` | 7464 | `TryBuildRuntimeGroupPalette` 的直接调用者 |
| `bool TryBuildRuntimeGroupPalette(const ShadowModelResourceRecord& resource,` | 6608 | 缓存/快照/arena 分配 |
| `kSemanticBuildChunkMaxRecords = 8u` | 9542 | 分块上限 8 条记录 |
| `buildWork->nextRecordIndex = m_core.buildFrameChunk(` | 9548 | 构建推进点（注释要求 render thread） |

### 8.3 `src/d3d9/war3/shadow/war3_shadow_renderer_core.h`

| 锚 | 行 | 证明什么 |
| --- | --- | --- |
| `uint64_t QueryPaletteSlotCacheSlotRangeStaleRejectCount();` | 448 | 导出访问器声明（接线锚点） |
| `enum class ShadowDrawPath : uint8_t {` | 17 | `Rigid=0, Skinned=1` |

### 8.4 `src/d3d9/war3/model/war3_model_hook.cpp`

| 锚 | 行 | 证明什么 |
| --- | --- | --- |
| `struct BlendedPaletteEntry {` | 295 | 非原子 `Matrix4 matrix` + `frameTag` + `valid`（§4 竞争面） |
| `static constexpr uint32_t kSlotBlendedPaletteCacheSize = 65536u;` | 302 | 槽位缓存容量 |
| `s_slotBlendedPaletteCache = {};` | 304 | 声明（`std::array`，非原子） |
| `struct RenderablePartPaletteBindingEntry {` | 313 | producer 绑定表结构（含 `sealedSelection`） |
| `kRenderablePartPaletteBindingCacheSize = 8192u` | 337 | 绑定表容量 |
| `kRenderablePartPaletteSnapshotMaxCount = 64u` | 338 | 快照上限单源 |
| `bool TryReadCurrentPaletteFrameTag(uint32_t& outFrameTag)` | 353 | 当前帧号来源（`g_gameBase + 0xBDA4CC`） |
| `bool CaptureBlendedPaletteSlotRange(uint32_t startSlotIndex,` | 2369 | **写入** `s_slotBlendedPaletteCache` 的唯一函数 |
| `void RecordRenderablePartPaletteBinding(` | 2480 | part→slot 绑定写入 |
| `void CaptureRuntimeGroupPaletteBindings(` | 2580 | FROZEN 携带逻辑所在 |
| `// Phase 7.52 根因修复：FROZEN 段里 War3 引擎的 8-帧 slot cadence` | 2638 | **自述"通常"是假设**（缺口文字依据） |
| `if (slotIndex == 0xFFFFFFFFu \|\| slotIndex >= 0x3A98u) {` | 2654 | 沿用 bindings 表上次 slot 的 FROZEN 携带分支 |
| `RecordRenderablePartPaletteBinding(partPtr, slotIndex, groupCount, frameTag,` | 2691 | 以**本帧 frameTag** 记录绑定 |
| `void __fastcall Hook_RuntimeMatrixWrite(int nodePtr, int sourceMatrixPtr,` | 7921 | 写入 hook（**不知 part**，只有 nodePtr + destMatrixPtr） |
| `CaptureBlendedPaletteSlotRange(` （writer hook 内） | 8020 | 第二个写入调用点 |
| `bool QueryBlendedPaletteBySlotIndexBestEffort(` | 9166 | 无锁读（best-effort，不用于仲裁） |
| `bool QueryCurrentPaletteFrameTag(uint32_t& outFrameTag)` | 9197 | A4 的当前帧查询 |
| `bool QueryBlendedPaletteFrameTagRange(uint32_t slotIndex,` | 9201 | A5 的区间查询 |
| `const auto& entry = s_slotBlendedPaletteCache[slotIndex + i];` | 9220 | **非原子读**（§4 核心） |
| `bool QueryRenderablePartPaletteSlot(void* renderablePart,` | 9240 | A0 的绑定查询实现 |
| `bool QueryRenderablePartPaletteSnapshot(void* renderablePart,` | 9285 | 快照查询实现 |

### 8.5 `src/d3d9/war3/model/war3_model_hook.h`

| 锚 | 行 |
| --- | --- |
| `bool QueryCurrentPaletteFrameTag(uint32_t& outFrameTag);` | 488 |
| `bool QueryBlendedPaletteFrameTagRange(uint32_t slotIndex,` | 490 |
| `bool QueryRenderablePartPaletteSlot(void* renderablePart,` | 499 |
| `bool QueryRenderablePartPaletteSnapshot(void* renderablePart,` | 507 |
| `bool QueryRenderablePartOwnerRuntimeModel(void* renderablePart,` | 521 |

### 8.6 `src/d3d9/war3/render/war3_skin_palette_selection.h`

| 锚 | 行 | 证明什么 |
| --- | --- | --- |
| `struct Selection {` | 20 | 既有对象级身份词汇（runtimeModel/part/ownerEpoch/publicationTicket/captureSerial/hash/slot/actualGroupCount/frameTag） |
| `slotAllocationGeneration stays zero until an actual native allocator witness exists` 注释 | 16-19 | **自证标签 ≠ 所有权**（§3.2 的纪律依据） |
| `inline bool OwnedSnapshotMatches(` | 37 | 既有"同帧同 slot"判定先例 |
| `inline bool CanReplace(` | 54 | 身份替换的既有约束 |

### 8.7 `src/d3d9/d3d9_device.cpp`

| 锚 | 行 | 证明什么 |
| --- | --- | --- |
| `bool War3SemanticPublishRegistriesBeforeSceneRuntime()` | 1223 | `DXVK_WAR3_SEMANTIC_PUBLISH_REGISTRIES_BEFORE_SCENE` 读取点 |
| `uint64_t War3SemanticContractCapturePeriodRuntime()` | 1230 | `DXVK_WAR3_SEMANTIC_CONTRACT_CAPTURE_PERIOD`（默认 240）——**禁止用作本工单预算** |
| `std::memcpy(event.label.data(), "skin-selection/v1", 18);` | 23149 | **复用 Kind + 独立 label 的既有先例** |
| `if (skinned && dxvk::war3::render::skin::ContractEnabled() &&` | 23141 | 该先例的准入条件（合同 ON ⇒ 生产 OFF 时零事件） |
| `event.data = {uint64_t(s.source), s.runtimeModel, s.part, s.ownerEpoch,` | 23154 | 对象级字段塞入 `data[12]` 的既有做法 |

### 8.8 取证基础设施

| 文件 | 锚 | 行 / 说明 |
| --- | --- | --- |
| `war3/tools/war3_frame_evidence_core.h` | `struct Event {` | 23；`key`+`thread`+`label[32]`+`data[12]`+`bits[48]` |
| 同上 | `class Ring {` | 44；`storageBytes`（46）、`arm` 容量上限 262144（50）、`visitFrozen`（126） |
| `war3/tools/war3_frame_evidence.cpp` | `json summary(const Snapshot& s,const Store& owner)` | 36；header `schema:7` + `effectiveConfiguration` |
| 同上 | `json eventJson(const Event& e)` | 52；序列化格式（`data` 字符串数组 + `words32`） |
| 同上 | `std::wstring outputPath(const Store& owner,uint64_t generation)` | 64；文件命名 `cpu-<pid>-<nonce>-<gen>.json` |
| 同上 | `bool createNew(` | 69；流式写、64 KiB 批量 flush、部分文件保留 |
| 同上 | `uint64_t Record(uint64_t session,Event event) noexcept` | 167；QPC+thread 戳、`catch(...)` 丢弃计数 |
| 同上 | `json Control(const json& payload` | 225；动作集、容量 256..262144（250）、内存准入 |
| `war3/tools/war3_frame_recorder_config.h` | `inline constexpr uint64_t RecorderDiskHeadroom=6ull*1024*1024*1024;` | 22 |
| `war3/tools/war3_frame_recorder_profile.h` | `ExternalRecorderProfile` / `InternalHighPressureRecorderProfile` | 20 / 24；cpuEvents 262144 / 65536 |
| `war3/tools/war3_frame_evidence_control.h` | `bool ClaimLocalRecorder(RecorderControlLease& lease) noexcept;` | 10；控制权租约 |
| `war3/tools/war3_control_plane.cpp` | `if (command == "frame_evidence") {` | 4568；`evidence::Control(payload)`（4572） |
| `war3/tools/war3_frame_history.cpp` | `evidence::Control({{"action","arm"},{"capacity",profile.cpuEvents}}` | 193；自包含采集器 arm/export（186-222） |
| `war3/tools/war3_frame_history.cpp` | `bool HandleFrameHistoryShortcut(` | 319 |
| `d3d9_window.cpp` | `tools::HandleFrameHistoryShortcut(WM_KEYDOWN,'C',true,true,false)` | 104（Ctrl+Shift+C）；Ctrl+F1 面板在 108-111 |
| `war3/ui/war3_imgui.cpp` | `void War3Imgui::drawFrameRecorderPanel() {` | 173；面板文案与"完全停用取证"说明（180） |
| `war3/tools/war3_internal_test_api.cpp` | `const auto recorder=QueryFrameHistoryHud();` | 314；目前无 evidence 命令（**待验证**） |

### 8.9 线程与构建门

| 文件 | 锚 | 行 | 证明什么 |
| --- | --- | --- | --- |
| `war3/render/war3_shadow_runtime_bridge.cpp` | `activeOwner != uint64_t(::GetCurrentThreadId()) \|\|` | 655 | 既有线程见证模式（配对发布） |
| 同上 | `capture.ownerThreadId != uint64_t(::GetCurrentThreadId())` | 676 | 同上 |
| 同上 | `// pipe 请求会把 buildFrameChunk 压到控制线程上` | 5313-5317 | **构建线程是运行时属性**（§4.2 依据） |
| `hooks/war3_hook_lifecycle.h` | `DWORD GetMainLoopThreadId();` | 53 | 主循环线程的第二见证 |
| `war3/core/war3_semantic_shadow_gate.cpp` | `bool IsSemanticSceneSubmissionRuntimeEnabled()` | 49 | 构建模式总门 |
| 同上 | `EnvFlagOrDefault("DXVK_WAR3_SEMANTIC_SHADOW_ENDFRAME_BUILD", false);` | 71 | `ENDFRAME_BUILD` 不得外推为默认生产路径（Q-D） |
| `war3/render/war3_shadow_runtime_bridge.h` | `semanticSceneSkinnedPaletteSlotCacheShadowCoreSlotRangeStaleRejectedCount` | 914 | 计数导出链的字段命名先例（10 个字段 905-915） |

### 8.10 测试与构建锚

| 文件 | 锚 | 说明 |
| --- | --- | --- |
| `AutoTest/test_recorder_event_wire_golden.py` | `raise WireError("EventValue", f"kind {ev.kind} out of range 1..18")` | **kind 被钉死在 1..18** ⇒ **已裁定（裁定 2）复用 `Kind::ShadowState`**，不改 wire |
| `AutoTest/test_palette_slot_cache_counters_export_static.py` | （FIELDS 5→10、访问器 3→8 的既有形态） | 新计数的导出门禁模板 |
| `AutoTest/test_palette_slot_cache_producer_confirmation_static.py` | A3/A4/A5 与区间上界断言 | 改 A 链时必改；本工单**不改** |
| `AutoTest/test_frame_recorder_memory_static.py` | 内存准入 | 复用环时须保持通过 |
| `src/d3d9/meson.build` | `test('war3_frame_evidence_runtime', war3_frame_evidence_runtime_test,` | 生产函数测试目标先例（约 296-306 行区） |
| `meson_options.txt` | `option('warvk_internal_frame_recorder', type : 'boolean', value : false,` | 内部取证构建开关（默认 false） |

---

### 8.11 本轮裁定新增锚点（2026-09-17 修订 R2 只读核对）

| 文件 | 锚 | 行 | 证明什么 |
| --- | --- | --- | --- |
| `war3/tools/war3_frame_recorder_config.h` | `struct RecorderConfiguration {` | 6 | 子门必须并入的配置面（`enabled/inputs/draws/local/internalBuild`） |
| 同上 | `ResolveRecorderConfiguration(...)` | 14 | `inputs/draws = enabled && flag` ⇒ 主门单独开启不带输入池（裁定 9） |
| `war3/tools/war3_frame_evidence.cpp` | `const RecorderConfiguration& recorderConfiguration()` | 121 | 主门/子门 env 的唯一解析点（禁止 core 内隐藏入口） |
| 同上 | `effectiveConfiguration` 导出块（`frameEvidence/rawInputs/skinPaletteContract/localRecorderOwner`） | 45 | effective configuration 导出面（加子门即改此键集） |
| `AutoTest/analyze_frame_evidence.py` | `set(config)==` 硬断言（4 键） | 44 | 解析器对 config 键集是硬断言 ⇒ 必须同步改 |
| `AutoTest/test_analyze_frame_evidence.py` | `effectiveConfiguration=dict(frameEvidence=True,...)` | 23 | 夹具键集，同上 |
| `war3/model/war3_model_hook.cpp` | `static std::atomic<uint64_t> s_renderablePartPaletteSnapshotSerial{0u};` | 343 | 已存在的发布序号候选（裁定 4：只记已算好的摘要/序号） |
| `war3/render/war3_skin_palette_selection.h` | `uint64_t ownerEpoch,publicationTicket,captureSerial,hash;` | 25 | `lifecycleIdentity` 的既有可证明来源候选（§2.1） |
| 同上 | `uint64_t slotAllocationGeneration=0;` | 26 | 仍为 0（无 native allocator witness）⇒ 不能当所有权（§5.6 ③） |
| `war3/shadow/war3_shadow_renderer_core.cpp` | `static std::atomic<uint64_t> g_semanticBuildOffThreadRefusedCount{0};` | 784 | 线程修复第一部分的证据面（§4.5） |
| 同上 | `g_semanticBuildOffThreadRefusedCount.fetch_add(1u,` | 9430 | 推进边界拒绝点（非主循环线程） |

---

## 附录 B：`[待验证]` 汇总（实施前必须只读核实）

> **2026-09-17 修订 R2**：本附录保留全部原始待验证项，并按本轮十项裁定与 K1-K6 增补。
> 标 **[裁定已定]** 的项已不再是“待上级裁定”，而是“实施前必须核实 / 冻结”。

**原 10 项（保留可追溯）**：

1. `PoseKernel` 命中是否有可用的 pose 发布序号；若无，`hitKeyPayload` 的降级形式。
   **增补 [裁定已定]**：裁定 4 下**不得**新增矩阵扫描哈希，降级形式只能是 `0 + digestUnavailable`（§2.1）。
2. `d3d9_device.cpp` 侧 Gap A 的复核是否与 shadow-core 侧针对**同一对象**、是否**同帧**；
   两侧计数器是否同一分母。（仍待核实；§2.1 已按"不假定同一分母"设计。）
3. `CGeosetData`（`Hook_RuntimeMatrixWrite` 的 `nodePtr`）→ `renderablePart` 是否存在
   可靠的既定映射（路线 B 记录"来源域"、路线 A 的前置）。**裁定 5 下 A 不批准**，该映射本轮**不必**提供。
4. `event.key.deviceEpoch` 在 shadow-core 的 A 链调用点当场是否可得。
   **增补**：取不到即 `0 + epochUnknown`，且该样本进 `identityWeak`（§2.1）。
5. `war3_internal_test_api.cpp` 能否承载 evidence 导出命令（第二个 dump 出口）。
   **裁定 7 下本轮不需要**：只复用现有 pipe/export。
6. JAPI `warvk:v1` 是否已有任何 evidence 相关命令（当前检索为无）。**裁定 7 [裁定已定]：不新增。**
7. 既有 P0 产物是否含可逐对象回溯的字段（Step 0 的结论）。**已闭合：无**（附录 D）。
8. `sizeof(Event)` 与 `sizeof(Cell)` 的实际编译期值（本文字段计算得 392 B）。
9. 写入侧线程戳应取"首个 writer"还是"最后 writer + 位掩码"。
10. 本工单 §2.5 的一次成功路径探测在实测帧时间中的量级。
    **增补（裁定 K3）**：还必须测"**子门关 / 开**"与"表空但门开"的成本（§6 Step 1 门禁）。

**本轮新增待核实（K1-K6 / 裁定 9 / 裁定 10）**：

11. `lifecycleIdentity` 在 A 链记录点当场**哪一个**既有契约可得
    （`Selection.ownerEpoch/publicationTicket/captureSerial` 或模型 immutable generation）——须在 Step 1 冻结；
    两者都不可得时的 `identityWeak` 比例必须在报告中单独列出。
12. 「实际提交」与「实际绘制」两级证据的**采集点**是否存在、在哪一行
    （§5.2 第 3、4 条目前只是"要求"，不是"已具备"）；若不存在，Step 1 必须冻结新增采集点或明写"未覆盖"。
13. `manifestFrameSerial` 在 A 链记录点当场是否可得（§2.1 的三帧分列依赖它）。
14. 共享环实际淘汰率与终态保留预算是否足够（§2.2 的 5 个丢失计数器是否覆盖全部丢因）。
    **R3**：终态 512 条从 4096 总计上限中**预留**（§R.5 A-1）；"缺链"必须由分析器**可判定**（§R.5 A-3）。
15. 子门加入 `RecorderConfiguration` 后，`effectiveConfiguration` 键集合变更对
    `analyze_frame_evidence.py` / `test_analyze_frame_evidence.py` / `test_frame_recorder_default_policy_static.py`
    的完整影响面（须逐测试列出）。
16. 覆盖路径结论（§1.5）：Step 1 落地后该路径是否仍 `resolved=0`；若仍为 0，报告只能写"未覆盖"。

---

## 附录 C：无代码变更声明

本文档为纯方案稿。**未修改任何 `src/`、测试或 `meson` 文件，未构建、未部署、
未启动游戏、未执行任何 git 写操作。** 全文所有结论均为对本树现有源码与既有台账的
只读引用；任何"已证明"字样均不出现在本工单对系统行为的描述中。

**2026-09-17 修订 R2 补充**：本轮修订同样**只改本文档**（B 树
`docs/plan/2026-09-17-p0-object-level-evidence-workorder.md`）；未改源码、测试或 `meson`，
未构建、未部署、未启动游戏、无 git 写操作。本文对上级十项裁定与 7 条关键点的落点声明
只是**授权范围**，不构成任何"已实施 / 已验证"结论。

**2026-09-17 修订 R3 补充（03:05 上级裁定）**：本次修订同样**只改本文档**；未改源码、测试或 `meson`，未构建、未部署、未启动游戏、无 git 写操作。修订内容仅限 §2.2 / §2.5.4 / §2.4 / §6 Step 1 的**预算总额口径**与**分析器缺链门禁**；4096 / 512 的数值本身、Step 顺序与 §5.6 五层口径均未改动，旧运行证据一字未改。

---

## 附录 D — Step0 只读回溯结果（2026-09-17 主线程执行；结论：现有产物无对象级证据）

对 `C:\Windows\Temp\warvk_p0\round2_on_m2` 的 `boundary_02_phase2_end.json`（627 键全量快照）
与 `shadow_summary_02_phase2_end.json` 做只读回溯，逐条结论：

1. **采集器未挂载**：`numericCounters.shadow.shadowEvidenceCollectorAttached = 0`、
   `shadowEvidenceRetentionRevision = 0` ⇒ M2 矩阵（`DXVK_WAR3_FRAME_EVIDENCE=0`）下
   **没有任何对象级事件被记录**。现场目录 `E:\Work\War3` 也没有 `cpu-<pid>-…json` 证据 dump
   （只有 2026-07 的 LAA 分析文件与 09-12 的 hotspot manifest），`war3_d3d9.log` 无 evidence 行。
2. **净结论**：现有三轮产物**不能**回答「某次拒绝属于哪个对象 / 之后是否被接住 /
   压力解除后是否恢复」——这正是本文 §1 的问题定义；必须按 §2 挂**有界**对象级记录后另开一轮。
   **不得**用现有聚合计数冒充对象级证据。
3. **顺带发现的既有计数线索（与卡点 1 直接相关，建议列入下轮观察面）**：

   | 键（boundary_02 实测值） | 含义 / 待查 |
   | --- | --- |
   | `paletteCaptureInvalidEntryMissCount = 1,798,896` | 捕获路径上「条目无效」的巨量 miss；与 `paletteCaptureExactHitCount=0`、`BestEffortHitCount=0` 并存 ⇒ 该读取路径几乎从不命中 |
   | `currentDrawCapturedPaletteQueryAttemptCount = 437,756` / `…HitCount = 437,756` | draw-time 捕获查询同点 Attempt==Hit（口径是否同分母待确认） |
   | `renderablePartPaletteSnapshotCapturedCount = 0`、`runtimeSimpleGroupPaletteSlotCapturedCount = 0` | producer 侧快照/槽位捕获为 0 ⇒ 与 Gap B 的键缺失链条一致 |
   | `semanticSceneDirectPaletteCaptureTrustedSourceHitCount/MissCount = 0/0` | trusted capture 路径在 ON 轮无活动 |
   | `semanticContractCaptureCalls = 14,418` | 契约捕获调用量级（可用于估算子门开销） |

4. **对裁定问题的影响（已被 D.2 取代，保留可追溯）**：§7 的 10 项裁定中，第 4 项（是否记内容哈希）与第 6 项（JAPI 出口）
   在 Step0 之后可以更便宜地决定——任何对象级记录都必须先解决 §2 的容量与「失败即关闭」预算；
   而 `FRAME_EVIDENCE` 主门当前是关的，故子门设计与主门取舍必须一次说清，不能靠「先跑再看」。

### D.2 本轮裁定后的回溯项更新（2026-09-17 修订 R2）

1. **覆盖路径（K6）**：D.1 的「采集器未挂载」与执行记录 §2.3 的 `resolved=0` 合并后的结论是——
   **旧轮既没有对象级事件，也没有执行到本工单要覆盖的路径**。
   因此本轮修订把"覆盖路径"从隐含假设升格为显式前置（§1.5 / §6 Step 0-1）。
2. **对原 §7 第 4、6 项的影响（保留原文）**：~~第 4 项（是否记内容哈希）与第 6 项（JAPI 出口）
   在 Step0 之后可以更便宜地决定~~ —— 两项均已被上级裁定：
   第 4 项**本轮不批准新增矩阵扫描哈希**；第 6 项**不新增 JAPI**（§7-4 / §7-7）。
3. **顺带线索的处置**：D.1 第 3 条列出的 `paletteCaptureInvalidEntryMissCount=1,798,896`、
   `currentDrawCapturedPaletteQuery Attempt==Hit==437,756` 等仍是**聚合计数**，
   不得作为对象级证据（§5.4）；只能作为"下轮观察面"与"覆盖路径是否活跃"的旁证。
4. **仍未闭合**：D.1 全部条目均为 2026-09-17 的单次只读回溯，未重跑、未复算；
   本轮修订不改动其数值，也不把其结论外推到其它矩阵（M1 / M2）。
