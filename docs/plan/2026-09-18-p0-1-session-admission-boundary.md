# P0-1 会话接纳边界 — 2026-09-18（未提交 / 未部署 / 未晋升稳定）

## 1. 缺陷（外部审核 Astra 实测，已由我独立复核）

```
war3_frame_evidence.cpp arm 分支原顺序：
  :362 setPreFreezeHook(&PaletteObjectPreFreezeHook)
  :364 s->active.store(s->generation, std::memory_order_release)   ← 发布
  :367 ArmPaletteObjectEvidence(s->generation, 0u)                 ← 记录器初始化
```

⇒ **记录器初始化晚于 active 发布**。后果（Astra 用真实新 sink + 记录器 + 受控线程调度复现）：

```
初始化前：已经接纳 FirstSight，wire 事件数为 0
初始化后：FirstSight 计数被清零，wire 仍为 0，**丢失计数也为 0**  ⇒ 无声丢失
```

第二形态（Astra 实测）：旧会话取得的对象键延迟到新会话执行 ⇒ 真 sink 输出
「顶层 session = 2 / 对象键 sessionGeneration = 1」⇒ **串会话事件**。
根因是记录器**没有在接纳时拒绝旧会话键**。

## 2. 我的独立复核

- 顺序：读源码确认 `:362 → :364 → :367`，Astra 正确。
- 生产键来源：4 个生产调用点**全部**用 `ActiveSession()` 构造键
  （`d3d9_device.cpp:22142` / `:23524`、`d3d9_war3_shadow.cpp:5234`、`shadow_renderer_core.cpp:6822` 附近），
  而 `ArmPaletteObjectEvidence(s->generation)` 用的是同一发布值 ⇒ **生产上两者同源**，
  因此「键的会话代际必须等于记录器当前会话」在生产上成立。

## 3. 修复（三处）

1. **重排**（`war3_frame_evidence.cpp`）：`ArmPaletteObjectEvidence(...)` 移到 `s->active.store(...)` **之前**；
   现在 Arm 在前、发布在后 ⇒ 发布后不存在「未初始化」窗口。
2. **接纳边界**（`war3_palette_object_evidence.h`）：新增 `AdmitsSession()` / `RejectIfStaleSession()`，
   在 6 个入口（`NoteReject/NoteFirstSight/NoteServed/NoteEnqueued/NoteDrawn/NoteObjectGone`）的
   `m_windowClosed` 早退后插入检查；被拒 ⇒ `m_counters.droppedStaleSession++` 并返回。
   规则：键未携带会话（0）⇒ 放行；否则必须等于 `m_sessionGeneration`。
   `droppedStaleSession` 是**内部计数、不写入 wire**（沿用既有 `windowMismatchLookups` 先例，避免触碰版本表）。
3. **夹具现实化**（这是契约修正的必要部分，不是放宽判据）：
   · `war3_palette_object_evidence_test.cpp` 的 `MakeKey`：原硬编码 `sessionGeneration = 1000u`
     ⇒ 改为 `Recorder().sessionGeneration()`（与生产同源）。
   · `war3_palette_object_evidence_cost_test.cpp` 的 `MakeKey`：原硬编码 `0xC0570000u` 与各处
     `Reset(...)` 的实参不一致 ⇒ 改为 `0u`（该测试只测**成本与预算**，不模拟会话；
     会话不匹配的拒绝由 Case 36 覆盖）。

### 设计取舍（如实登记）

- 我最初还写了「会话代际为 0 ⇒ 一律不接纳」。它**破坏了** `default/unconfigured 仍推进状态机` 的既有契约
  （Case 1 具名失败）⇒ **已删除**。理由：记录器**不替调用方做 arm 门**；「发布早于初始化」那个窗口
  应由**控制路径的顺序**关闭（现在由 `check(_arm < _active)` 静态锁钉住），而不是在记录器里重复实现。
- **已知局限**：键若**未携带**会话代际（0）则不受接纳边界保护。生产采集点
  （`war3_palette_object_capture.h:159 key.sessionGeneration = sessionGeneration;`）**无条件写入**该字段，
  故生产不受影响；此局限已写在代码注释里。

## 4. 验证（全部检查退出码）

```
ninja -C build32 -j4                                   exit 0
ninja -C build32 -n                                    no work to do
AutoTest 静态全量                                        263 / 0
ninja -C build32 test                                  Ok 85 / Fail 0   (exit 0)
war3_palette_object_evidence_test.exe                  35 passed, 0 failed（新增 Case 36）
wire 驱动 test_palette_object_wire_roundtrip.py + EXE    CHECKS=1160 FAILURES=0
```

Case 36 实测输出：
```
[SESS-36] staleDrops=2 stored=0     ← 旧会话键（69，当前会话 70）两次调用均被拒，零事件
[SESS-36] fresh stored=2            ← 对照：当前会话键被接纳
[PASS] 36 stale-session admission boundary (P0-1) (3 checks, 0 failures)
```

## 5. 可失败性探针（两条，均**具名失败**）

```
探针 A（接纳边界失效）：把 AdmitsSession 末行改为 return true; ⇒
  [FAIL] 36 stale-session admission boundary (P0-1) (3 checks, 2 failures)
  FAIL: P0-1: BOTH stale-session calls must be rejected (FirstSight + Served)
  FAIL: P0-1: a stale-session key must produce NO event at all
探针 B（顺序退回旧形态）：把 Arm 移回 active 之后 ⇒
  FAILURE: D2/P0-1: the palette recorder MUST be initialized BEFORE active is published
           (otherwise events arriving in that window are silently lost ...)
```

两条探针都已还原并复跑通过（探针 B 用原文写回 + 逐字节相等校验）。

## 6. 静态锁修正（**旧锁漏了一半**）

`AutoTest/test_palette_object_arm_order_static.py` 原先只断言 `hook < active` 与 `hook < arm`，
**恰好漏掉**裁定要求的另一半「先初始化记录器再发布」⇒ 新增 `check(_arm < _active, ...)`。
**教训**：把「部分满足」当成「契约已锁」，比没有锁更危险 —— 它会把错误顺序固化成一种「规范」。

## 7. 不声称

```
· 不声称 Astra 指出的两种形态在前台/实机已消失（本修复只有宿主机与静态证据）
· 不声称实机行为已验证（本轮零实机采集）
· 不声称 disarm 路径（s->active.store(0)）也已同样审查完成 —— 仅初步看过，未做等价修复与探针
· 不声称全门禁通过（263/0 是 test_*_static.py 通配；门禁外文件另有 3 个既有红）
```
