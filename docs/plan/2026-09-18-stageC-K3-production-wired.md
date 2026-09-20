# 阶段 C · K3 生产侧接线落地 — 2026-09-18

> 裁定条目：「跨帧判序改为按一次明确关联的尝试（attemptSerial 按值携带），不采用每帧清零」。
> 本轮是 K3 的最后一环：机制早在 round 39 就位，但生产三个站点全传默认哨兵，
> 因此实机判序仍是终身单调，round 30 的反例在真实管线上仍然成立。

## 1. 「一次尝试」的语义与取值依据（不是猜的）

语义 = 一条清单记录所关联的那一次尝试（对象的清单记录更新 ⇒ 新的一次尝试）。

关键事实（实测）：拒绝点与服务点本来就共享同一个值族：

  war3_shadow_renderer_core.cpp:6821-6827 (Reject)
      recordFrameSerial = renderable.frameSerial          <- 清单记录自己的序号
      （同点还有 renderFrame = RenderState::getFrameIndex()，那是帧级索引）
  d3d9_device.cpp:22139-22143 (Served)
      recordFrameSerial = draw.shadowRecordFrameSerial
  d3d9_device.cpp:21516  ★ draw.shadowRecordFrameSerial = packet.renderable.frameSerial

⇒ packet.renderable.frameSerial 是拒绝点与服务点共享、按值携带的尝试身份；
它是记录级而不是帧级 ⇒ 不触犯「不采用每帧清零」（帧内同一对象可以有多条记录）。

## 2. 接线（零新增数据流）

工厂新增带默认值的第 5 参，三站点传各自本来就在传的 recordFrameSerial：

  MakePaletteObjectFrames(renderFrame, recordFrameSerial, nativeFrameTag,
                          nativeKnown, uint64_t attemptSerial = ~0ull)   // 默认 = 未知哨兵
    frames.attemptSerial = attemptSerial;

  Served     d3d9_device.cpp:22145                    draw.shadowRecordFrameSerial
  FirstSight d3d9_device.cpp:23524                    uint64_t(manifestFrame)
  Reject     war3_shadow_renderer_core.cpp:6830       renderable.frameSerial

⭐ 默认哨兵保持不变 ⇒ 既有调用方（测试）行为逐位不变，
Case 30 的哨兵契约锁仍然有效（它锁的正是「省略参数 ⇒ 与 K3 之前逐位相同」）。

## 3. 裁定的「生产调用方传参测试」

新 AutoTest/test_palette_object_attempt_serial_caller_static.py（自动入门禁）断言：
  (1) 工厂必须接收并真的写入 attemptSerial，且默认是哨兵；
  (2) 三个活的生产站点各自按值传真实序号；
  (3) 生产里不得显式传哨兵（那等于没接）；
  (4) 活源码里恰好 3 个调用点（AutoTest 归档副本不算）。

另加 evidence Case 33（走生产所用的那条路，工厂）：按值带号 / 不干扰 recordFrameSerial
/ 省略参数仍为哨兵（3 checks）。

## 4. 载荷性（两个探针，都具名失败）

  探针 A（撤掉 Served 点的尝试号）:
    FAILURE: K3: the Served site must pass draw.shadowRecordFrameSerial as the attempt serial
  探针 B（撤掉 FirstSight 点的尝试号）:
    AssertionError: 首见采集点必须按值携带 attemptSerial（K3）

## 5. ⚠️ 本轮弄坏并修好了一个既有测试（如实记录）

接线后静态全量由 261/0 变为 262/1：
test_independent_review_sep18_fixes_static.py:55 的
  assert "0u, false);" in _tail, "生产采集点必须传 nativeKnown=false"
失配。

诊断：该断言的意图（首见点 nativeKnown=false）我的改动完整保留了；
失配只因字面量 "0u, false);" 绑在了旧的实参个数上。

处置（加强，不是放宽）：
  原  "0u, false);" in _tail        →  新  "0u, false," in _tail   （不再依赖其后跟几个实参）
  原  "0u, true);" not in _tail     →  新  "0u, true" not in _tail  （无论其后跟什么，传 true 都会被抓住）
  新增                                  "uint64_t(manifestFrame));" in _tail（该站点必须携带 K3 尝试号）

⇒ 修改后 STATIC=262/0。没有删除任何断言，没有放宽任何期望。

## 6. 状态

  STATIC = 262 脚本 / 0 失败（259 + arm-order + header-snapshot + attempt-serial-caller）
  meson 85/0 ; no-work ; evidence 32/0 ; wire CHECKS=1160 FAILURES=0 ; 读方 OK
  DLL = 23A2EF1959DBFD07EB8C28035BC73394436BBFFE120CF665539A390B03AD25A1

  h war3_palette_object_capture.h = C8752ABFD766DB537030C81E3DE2D2A96CCF9EBCDC999F7823A798FC2C12944A
  h d3d9_device.cpp               = 0D91C98EED6324AB3BA643BF66770C10C7445696027F6FD22C415D7EC08C43A4
  h war3_shadow_renderer_core.cpp = E9F91341D934C601192BAB77F97219AE1F9D0A66E144D725D05381BDCD0C5082
  站点 = E:\Work\Warcraft III\d3d9.dll = F275545BAA65A015…（基线，未部署）
  git  = 无写操作

## 7. 不声称（重要）

- 不声称实机判序已改善 —— 本轮没有任何实机观测；接线消除的是
  「生产永远走未知哨兵」这一结构性事实，但「序号是否真的随清单记录更新而变化」
  需要在真实数据上核对（未做）；
- 不声称 manifestFrame（首见点）与 packet.renderable.frameSerial（服务/拒绝点）
  在实机上确实相容 —— 前者族属是 round 39 标记的「三点里最特别的一个」，
  只由代码结构支撑，未由数据支撑；
- 不声称 C 批次完成：归属规则其余阶段形状仍待裁定；
- 不声称两个 P0 完成 ⇒ 不新增实机因果结论、不晋升稳定候选；
- 全局边界：一张表不代表一种事实，一条链结算不代表观察完整，
  观察完整不代表对象已证明，更不代表阴影已恢复。