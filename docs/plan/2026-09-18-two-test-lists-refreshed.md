# 两份测试清单（**刷新版**）— 2026-09-18（round 65）

> 裁定要求「写方/读方/测试同步，并**分别列项**『生产编码往返测试』与『生产调用方传参测试』」。
>
> ⚠️ **本版刷新原因**：上一版写于 round 40。此后新增了 **4 把静态锁**（D2 顺序、D3 快照、
> K3 调用方、E 原因）与 **3 个宿主机用例**（B1 屏障、K3 工厂、窗口前置）⇒ 旧清单**已过期**，
> 继续引用它会构成误导。

---

## 清单 1 · 生产编码往返测试

**回答的问题**：*写方产出的 wire 字节，读方能不能按同一版本契约还原成同一条链？*

| 项 | 载体 | 现状 |
| --- | --- | --- |
| 生产 sink 往返载体 | `war3_palette_object_wire_roundtrip_test.cpp`（EXE） | 原生跑 **ROUNDTRIP checks=126 failures=0 PASS**（含 ENCODER 73 checks） |
| 门禁驱动 + **场景齐全**硬条件 | `AutoTest/test_palette_object_wire_roundtrip.py` | **CHECKS=1160 FAILURES=0**；`REQUIRED_SCENARIOS={A..G}`，缺一场即失败 |
| 场景 | A 四阶段强身份 / B identityWeak / C 环淘汰 / D 空样本 / E 并发所有者 / F post-window 自动冻结 / G 正常观察链 | G 必须**排在最后**（A–F 的序号被外部钉死值依赖） |
| 编码器单元 | 同上 EXE 内的 `RunEncoderUnitChecks` | ENCODER checks=73 failures=0 |
| 通用根读方 | `AutoTest/analyze_frame_evidence.py` + `test_analyze_frame_evidence.py` | **OK**（注：**不在** `test_*_static.py` 通配内，是手工补跑） |
| 控制面静态 | `AutoTest/test_frame_evidence_control_static.py` | 在静态全量内 |

**版本契约（往返测试的判据来源）**：
`paletteObject` 恒定 **v4**；v1/v2/v3 仍按各自版本解析；终态表与阶段表**按版本校验**。
v4 的记录级字段含 `data[4] = chainType`（v1–v3 该槽仍须为 0，**两侧**测试证明）。

---

## 清单 2 · 生产调用方传参测试

**回答的问题**：*生产**调用点**真的把值传进去了吗？*（**"机制已实现"与"生产线已接线"是两件事**。）

| # | 锁 / 用例 | 钉住什么 | 可失败探针 |
| --- | --- | --- | --- |
| 1 | `AutoTest/test_palette_object_attempt_serial_caller_static.py` | K3：工厂必须接收并**写入** `attemptSerial`（默认哨兵）；**三个活的生产站点**各自按值传真实序号；生产**不得**显式传哨兵；活源码里**恰好 3 个**调用点（归档副本不算） | ✅ 撤 Served 点 ⇒ `FAILURE: K3: the Served site must pass draw.shadowRecordFrameSerial…`；撤 FirstSight ⇒ 具名 AssertionError |
| 2 | `AutoTest/test_palette_object_native_reason_static.py` | E：`nativeKnown` 必须**按值携带**（来源判定处 + 新鲜度判定处各赋值），**不得**由 `selectedPalette.frameTag` 反推；赋值点须与新鲜度判定同处 | ✅ 把反推放回 ⇒ `FAILURE: E: nativeKnown must not be re-derived from selectedPalette.frameTag…` |
| 3 | `AutoTest/test_palette_object_capture_points_static.py` | 五个采集点的既有契约（round 早期） | 在静态全量内 |
| 4 | 宿主机用例 **33**（`war3_palette_object_evidence_test.cpp`） | K3：走**生产所用的那条路**（工厂）—— 按值带号 / 不干扰 `recordFrameSerial` / **省略参数仍为哨兵** | 3 checks |
| 5 | 宿主机用例 **34** | 窗口维度前置：`Reset` / `ResetForSessionTransition` 必须清表；**且**查找不得遇到**别的窗口**的条目（`windowMismatchLookups == 0`） | ✅ **完整**"跨窗口保留条目"变异 ⇒ 用例**具名失败**；（只删清表循环、仍清 bloom 的变异**不**触发 ⇒ 见下方限定） |

### 清单 2 的**准确界限**（不夸大）

- 第 5 项的守卫**只在记录器内部可见**：`windowMismatchLookups` **不上 wire** ⇒
  **实机上的这类违规不会出现在导出里**（要让它 fail-visible 需裁定 v5 或 v4 加字段）；
- 第 5 项对"**保留条目但仍清 bloom**"的变体**不敏感**（那种情形造成的是**孤儿条目占槽**，
  属于另一个缺陷），这一点已实测并记录。

---

## 附加（不属于裁定要求的两份清单，但**不得隐瞒**）

| 项 | 载体 | 钉住什么 |
| --- | --- | --- |
| 读方规则套件 | `test_palette_object_evidence_analysis_static.py` | 链型槽版本门控、Observation 不得用 Recovered、Observation 不得带 Rejected 阶段、拒绝链不得带 FirstSight、**拒绝链不得用 ObservationClosed**（终态×链型矩阵封口） |
| D2 顺序锁 | `test_palette_object_arm_order_static.py` | 预冻结钩子必须**先于** `active` 发布与 arm |
| D3 快照锁 | `test_palette_object_header_snapshot_static.py` | 头块**恰好一次**快照；版本是**字面量 4**（无动态形式） |
| 宿主机用例 **32** | `war3_palette_object_evidence_test.cpp` | B1：armed 的发布是单一同步域（5 checks；位于 wire EXE 内**仅为链接原因**） |
| 宿主机用例 1–31 | 同上 | 记录器语义（预算、终态、链型、归属、K1/K2/K3 等） |

---

## 当前全量数字（round 63 单跑，与交付包同一棵树）

```
AutoTest 静态全量      263 脚本 / 0 失败   （6 把 palette 锁 + 其它）
meson test             Ok: 85 / Fail: 0
palette 宿主机测试      33 passed, 0 failed
wire 原生              ROUNDTRIP checks=126 failures=0 PASS
wire 驱动              CHECKS=1160 FAILURES=0
读方 analysis static   OK（106 tests）
读方 analyze_frame_evidence  OK（不在静态通配内）
```

## 不声称

- **不**声称"全门禁通过"（裁定禁止）；
- **不**声称这些测试在**实机**上运行过（全部离线/宿主机）；
- **不**声称清单**已穷尽**（它列的是当前树上实际存在的项；新增测试必须同步更新本文）；
- 全局边界：一张表不代表一种事实，一条链结算不代表观察完整，
  观察完整不代表对象已证明，更不代表阴影已恢复。
