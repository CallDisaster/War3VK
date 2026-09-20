# 目标逐条**完成台账**（证据 + 状态）— 2026-09-18（round 77）

> 目的：把目标的**每一条**与「满足它的证据」配对，并标明**状态**，
> 使**五项待裁定**一次性可决策。
>
> 口径纪律：**不声称全门禁通过**；**不声称**任何实机因果结论；
> 隔离桌面数据不当前台性能；`E:\Work\War3` **从未被触碰**。

## 0. 总状态

```
A  文档更正版            ✅ 已完成（冻结包内层四指纹逐项未变，外层新 SHA）
B  批次1-a 门禁恢复      ✅ 已完成（5 个可失败性探针，失败项具名）
C  批次1-b 链型与 v4     ✅ 离线部分全部完成；1 项语义待裁定（双链路由）
D  批次2 会话协议        ✅ 已完成（三条屏障 + D2/D3 静态锁）
E  受控实机              🟡 代码半已完成；**实机半仅完成预备装置**，采集需授权
```

## 1. A · 文档更正版

| 项 | 证据 |
| --- | --- |
| 交付包 | `WarVK-delivery-20260918-doc-r1.zip` 32.41 MB |
| 包哈希 | `7619E19568F9E0B821BA20164C5C1695CCEFAF6777FDC69E5AC91DEC2D792481`（**本轮复核与记录一致 ⇒ 仍不可变**） |
| 内层四指纹 | 逐项未变（记录于 `2026-09-18-delivery-package-index.md`） |

## 2. B · 批次1-a：恢复真正会失败的门禁

| 裁定条款 | 证据 |
| --- | --- |
| `check_scenario_g` 移出注释 | `AutoTest/test_palette_object_wire_roundtrip.py`（+42/−16） |
| `REQUIRED_SCENARIOS={A..G}` 成为硬条件 | 同上；**场景齐全**已硬校验 |
| 5 个可失败性探针 | 记录于 `2026-09-18-astra-stageB-gate-restored.md`；其中一次探针是**哑弹**（recorder 恰好未 arm）并已重做 ⇒ 探针可信 |
| 此检查点允许为红、失败须具名 | 已遵守；**未**放宽期望、**未**删断言、**未**登记错误期望 |
| 包 | `stageB-gate.zip` 78,436 B `8DDB25E3642DB7CEC47EFC5A7FCE8B78C75EC75C7E19684318F1E1880997893E`（9 条目，**无 DLL**）；一致性 7/7 |

## 3. C · 批次1-b：链型与 v4

| 条款 | 证据 | 状态 |
| --- | --- | --- |
| 共用有界存储与预算 | 预算常量与 Case 7（64/帧、3584、4096、512） | ✅ |
| 两类链**独立且不可改类**的条目 | `FindChain` 谓词 `SameKey && chainType==type`（不可改类）；Case 27 | ✅ |
| 同一对象可同时持有两条链 | Case 27、Case 35 | ✅ |
| 查找维度 = 对象键×链型×**窗口** | `Entry::windowSegment` + 谓词 + `windowMismatchLookups`；Case 34（含**完整**保留变异探针具名失败） | ✅（**导出层可见性待裁定**） |
| 两类入口都先 `CanEmit` 再 `Insert` | Case 25（拒绝入口）、Case 29（首见入口） | ✅ |
| 跨帧判序 = 一次明确关联的尝试（按值携带） | `attemptSerial` 参数贯穿；生产三点接线；Case 30/31/33 | ✅ |
| **不采用每帧清零** | ≤1 终态/对象/窗口 + `attemptSerial` 按**清单记录**携带 ⇒ 非帧级 | ✅ |
| 终态 `ObservationClosed=7`（0–6 不动） | `TERMINALS_OBSERVATION_CLOSED=7`；`TERMINALS` 0–6 原样 | ✅ |
| Observation 永不使用 `Recovered` | 读方规则 `observationChainMustNotUseRecovered` + 用例 | ✅ |
| `Recovered` 必须有真实拒绝事实 | Case 26（K2） | ✅ |
| 新写方一律 **v4**，删 `firstSightUsed()` | `PaletteObjectHeaderJson` 字面量 4；静态锁断言 `firstSightUsed() ? 3 : 2` **not in** | ✅ |
| v1/v2/v3 仍按其版本解析 | `TERMINALS_BY_VERSION` + `LEGACY_STAGES` + 链型槽版本门控用例 | ✅ |
| 终态表**与阶段表**必须按版本校验 | 终态：`test_observation_closed_terminal_is_version_gated`；阶段：`test_first_sight_stage_is_rejected_wholesale_under_legacy_version`（**双向**） | ✅ |
| 修我引入的误拒不变量（710/716 未排除终态） | `analyze_frame_evidence.py` +8/−0（K1） | ✅ |
| 写方/读方/测试同步 | 静态锁 6 把 + 宿主机 34 用例 + wire 1160 检查 | ✅ |
| 分别列项两份测试清单 | `2026-09-18-two-test-lists-refreshed.md`（含**准确界限**：窗口计数不上 wire；对"保留条目但清 bloom"变异不敏感） | ✅ |
| 包 | `stageC-chaintype.zip` 8,966,060 B `FF68406D2FA116499AB61D971B3E3EE4F92712594F08E9F23523A485792DA30B`；一致性 18/18 | ✅ |

### ⚠️ C 的两处**未闭合**（都在等裁定）

1. **窗口维度的导出层可见性**：内部守卫已落地（Case 34 载荷），
   但 `windowMismatchLookups` **不上 wire** ⇒ 实机上的跨窗口违规**不会出现在导出里**。
   选项：**v5**（窗口段进条目并导出）或 **v4 加字段**（与"恒定 v4 + 版本表"张力）。
2. **双链并存时的路由语义**（有**对照/处理实测**支撑，见下）：

```
对照（只有拒绝链）+ 完整序列 served→enqueued→drawn  ⇒ recovered=1        ✅
处理（两链并存）  + 同样的完整序列                 ⇒ recovered=0 windowExpired=1  ❌
逐记录：rec[2/3/4] stage=2/3/4 全在 chain=1（观察链）；rec[5] 拒绝链终态=WindowExpired
```

⇒ **同样的完整恢复序列，只因多一条观察链，拒绝链就丢失全部恢复事实、永远无法 `Recovered`。**
⇒ 选项 **B（改为优先拒绝链）实测只是把假阴性搬到观察链**（探针：`closedRecovered=1 closedWindowExpired=1`）⇒ **不是解**。
⇒ 若要修，语义上只能是 **C（两链各自独立记一份）**；它会改变**事件数与导出形状**。
⇒ 现状用**特征化断言**锁住（3 checks 全绿，消息明说"**不是正确性主张**"），
   任何人改路由都会被抓住（探针：`[FAIL] 35 … (3 checks, 2 failures)`）。

## 4. D · 批次2：会话发布·关闭·冻结·快照

| 条款 | 证据 |
| --- | --- |
| armed 与计数读取统一同步域 | `std::atomic<bool>` + `store/load(release/acquire)`；Case 32（B1 屏障，5 checks） |
| 先初始化记录器与预冻结钩子再 release 发布 active | 顺序锁 `test_palette_object_arm_order_static.py`（D2）+ 变异探针 |
| HeaderJson 计数与版本字段**同一快照** | `QueryPaletteObjectEvidenceHeader` 单次快照；`test_palette_object_header_snapshot_static.py`（D3） |
| 三条屏障测试 | 屏障用例 + `2026-09-18-stageD-verification.md` |
| 包 | `stageD-session.zip` 8,297,987 B `436367D6B46F32224D24DD004DFBED2BBA29DF8008F51A78F2AE3856A9F62E8A`；一致性 13/13 |

## 5. E · 受控实机

### 5.1 代码半 ✅

| 条款 | 证据 |
| --- | --- |
| 原因字段改为在**实际清空/拒绝分支**赋值并**按值携带** | 引入 `paletteObjectSelectionFromLiveNative`；`:22149` 的 `nativeKnown` 不再由 `selectedPalette.frameTag` 反推 |
| 覆盖五个点位 | `:20321`（初始选择）、`:20452`（替换）、`:20865-20870`（两个真实事实来源，**本计划改动处**）、`:21694-21697`（`drawTimeVBOverrideApplied=true` **已在该真实分支赋值**）、`:23299-23530`（FirstSight 硬编码 false）。**恰好一处需要修**，已修 ✅ |
| 静态锁 | `test_palette_object_native_reason_static.py`（探针：把反推放回 ⇒ **具名失败**） |

### 5.2 实机半 🟡 **仅完成预备**（无采集）

| 条款 | 证据 |
| --- | --- |
| 白名单**从源码机械枚举** | 必填 2 项（`DXVK_WAR3_FRAME_EVIDENCE`、`_PALETTE_OBJECT`，缺失 ⇒ 证据子系统**零操作**）+ 上下文旋钮 20 项 + **全量 `DXVK_*` 兜底**（理由：漏记本身就是盲点） |
| 保存完整启动环境 | `AutoTest/capture_startup_environment.ps1`（**只读**）⇒ `startup-env-<stamp>.json/.md`；**实跑验证**：变量名正确渲染、缺失必填项**具名**、站点 DLL 哈希与基线逐字相同 |
| ⚠️ 一条待核对观察 | 本机有**三个**显示适配器（RTX 4060 Ti / OrayIddDriver / MuMu Virtual Display）⇒ 存在**虚拟显示适配器**；这对"隔离桌面 vs 前台"的采集语境有直接影响。**记为观察，不据此下结论** |
| **未做** | 未部署 DLL、未启动游戏/编辑器、未覆盖任何 YDWE/Warcraft 文件、未设置任何环境变量 |

## 6. 硬约束逐条核对

| 约束 | 核对方式 | 结果 |
| --- | --- | --- |
| 两个 P0 完成前不新增实机因果结论 | 全部文档 | ✅ 零实机因果结论 |
| 不晋升稳定候选 | 全部交付物 | ✅ 无"稳定"声明 |
| 不声称全门禁通过 | 每轮报告 | ✅ 始终声明"不声称" |
| 不把隔离桌面数据当前台性能 | 无隔离桌面性能数据产生 | ✅ |
| 不使用 git 写操作 | 全程 | ✅（仅用只读 `git diff --no-index`） |
| 不得为通过而放宽判据 | 逐次改动核对 | ✅ 无放宽；K3 文字锁失配时选择**加强**而非放宽 |
| 不改变 palette 准入 / 几何选择 / 渲染优先级 | **机械 diff 对不可变基线包** | ✅ 9 文件、两渲染路径 hunk 逐条核对为**纯证据采集追加**（E 的局部布尔条件未变） |
| 不以关闭动态 caster / 减少正常阴影作为修复 | 改动范围 | ✅ 完全未触碰 caster 生成与阴影开关 |

## 7. ★ 五项待裁定（决策就绪）

| # | 事项 | 选项 | 证据/后果 |
| --- | --- | --- | --- |
| 1 | **窗口维度的导出层可见性** | (a) v5：窗口段进条目并导出 (b) v4 增字段 (c) 维持仅内部可见 | 内部守卫已载荷（Case 34）；不加则**实机跨窗口违规不可见** |
| 2 | **双链并存的路由语义** | (a) 维持现状（已特征化锁住）(c) 两链各自记一份 | **对照/处理实测**：现状下拒绝链丢失恢复；**B 已实测无效** |
| 3 | **E 实机采集授权** | 授权 / 不授权 | 装置**已就绪**；AGENTS.md 要求**用户明确请求**才能部署/启动 |
| 4 | **是否让构建可复现** | 是 / 否 | 已确证两个原因：PE `TimeDateStamp`（=链接时刻，实测相差 0 秒）+ `__DATE__/__TIME__`（`war3_perf_monitor.cpp:3839`）。`__DATE__/__TIME__` 大概率是**有意**诊断信息，去掉会改变性能报告内容 |
| 5 | **门禁外的三个红 + `d3d9_mem.cpp` 覆盖缺口** | (a) 由契约所有者更新锚点后纳入门禁 (b) 维持门禁外 | 三个红**均已诊断**（2 个测试过时 + 1 个环境性），且**与基线逐字相同** ⇒ 非本计划造成。**`d3d9_mem.cpp` 分配契约在 Python 门禁(263)与宿主机 meson(76)里都 0 覆盖** |

## 8. 当前可复核的全部数字（本轮实测）

```
ninja -C build32 -n                     no work to do
AutoTest 静态门禁                        263 / 0          （= test_*_static.py；另有 40 个不在门禁内）
meson test                              Ok: 85 / Fail: 0
palette 宿主机测试                       34 passed, 0 failed
wire 原生往返                            ROUNDTRIP checks=126 failures=0 PASS
wire 驱动                                CHECKS=1160 FAILURES=0
读方 analysis static                     OK
树 DLL  E2E0270A…(36,333,949 B)          ← 哈希含构建时刻，非源码指纹
站点    F275545BAA65A015…                 ← 与基线逐字相同 ⇒ 未部署
包 A    7619E195…（冻结、不可变）  包 B  8DDB25E3…  包 C FF68406D…  包 D 436367D6…
```

## 9. 不声称

- **不**声称 B/C/D 已"完成验收"（离线证据齐备 ≠ 实机/玩家前台验收）；
- **不**声称 E 有**任何**实机数据（**装置 ≠ 采集**；零实机观测）；
- **不**声称全门禁通过（且已量化："263/0"**不含**门禁外 40 个文件里的 3 个红）；
- **不**声称任何症状已被修复、阴影已恢复或候选可晋升；
- **不**声称五项待裁定我能自行决定（round 66 已证明我自己选会出错）；
- 全局边界：一张表不代表一种事实，一条链结算不代表观察完整，
  观察完整不代表对象已证明，更不代表阴影已恢复。
---

## ⚠️ 更正（round 80）：§3 中「删 `firstSightUsed()`」一行**措辞过窄**

该行写的是『新写方一律 v4，删 `firstSightUsed()` …… ✅』，读起来像**函数已被删除**。

**准确表述**：

```
动态选版本用法 = 已删除（生产代码零调用；写方 :85 恒 version=4）            ✅
函数本体       = 保留（war3_palette_object_evidence.h:696）                 ⚠️
                 且静态锁 test_first_sight_observation_chain_static.py:55
                 明确要求它继续存在 ⇒ 与裁定字面「删除 firstSightUsed()」
                 存在**解释分叉**（"删除用法" vs "删除函数"），需裁定。
```

详见 `2026-09-18-firstSightUsed-interpretation-and-stale-version-comments.md` §1。
因此**待裁定事项增加一项**（共 **六项**）。