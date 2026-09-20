# 2026-09-16 DSH（Kimi）：E1 64位 CPU 取证历史存储 HistoryStore

状态：候选已实现并完成纯 Python/静态验证；**未经编译器、native、跨进程或实机验收**。
范围严格限于合同 `docs/plan/2026-09-16-recorder-cpu-offload-contract.md` 的 E1 历史核心：
不接 IPC、磁盘、launcher 或产品；首轮只做 CPU 存储。共享 wire 头由主线程冻结/维护，本线程未改。

## 交付物（本批 4 路径，全部新增）

| 文件 | 内容 |
| --- | --- |
| `tools/render_host/recorder_history_store.h` | 公开接口：`HistoryStore`（`warvk::host::recorder`）、`StoreError`、`Summary`、`StoreState` |
| `tools/render_host/recorder_history_store.cpp` | 实现：accept 自 Decode、pre/post 分区、trigger 一致性、Seal 闭合、永久 Fault、冻结排序遍历 |
| `AutoTest/test_recorder_history_store.cpp` | 真实 Encode+HistoryStore 正反测试，输出 `checks=N PASS` 与 storageBytes |
| 本文件 | 设计与验证记录 |

文件 SHA-256 见末节回执（文档按惯例不自哈希）。

## 合同逐条映射

- `accept(const uint8_t*,size_t)` 内部自行 `Decode` 真实包；调用方 View 一律不信。任何无效包或
  申请失败 → 永久 Fault（`m_storeError` 保留首个原因，Fault 后 accept 返回原错误，不重开）。
- Begin：Empty 才接受；ordinal=1/count=0/无 trigger/无 totals；一次性 `make_unique<Event[]>(capacity)`
  申请；`pre=capacity-capacity/4`，post=余量；申请失败（bad_alloc 经 accept 捕获）→ AllocationFailed Fault。
- Data/Seal：session 精确匹配、capacity 不变、ordinal 必须等于 nextOrdinal（防跳号/重放），
  且 `nextOrdinal==UINT64_MAX` 显式 `OrdinalOverflow`（不回绕成合法 0）。
- Data：先全量验证（DecodeEvent 逐条、session、sequence 自 1 连续、post 下标范围）再统一提交，
  禁止半包写入。sequence 推进前 `h.count > UINT64_MAX-1-m_lastSequence` → `SequenceOverflow`，
  wrap 永不变合法。pre 落 `(seq-1)%pre` 且覆盖计 evicted；post 判定用
  `seq-cut-1 >= m_capacity-m_pre`（不先加 m_pre，无 uint64 回绕），超容量 `PostOverflow` 拒绝不覆盖。
- trigger：首次可在 Data 或 Seal 给出，`cut < 此前 lastSequence` → `TriggerRegressed`（不追溯重分类）；
  固定后每包必须完全相同——后续 Data/Seal 传不同值或 `NoTrigger` 均 `TriggerConflict`（本轮修订点 1）。
  Seal 另要求 `cut <= lastSequence`，否则 `TriggerNotCovered`。
- Seal：`reason 1..4`、`accepted==store实际处理数`、`attempted>=accepted`、`lost==attempted-accepted`、
  `lastSequence==accepted` 不变量；全部精确闭合否则 `TotalsMismatch`。成功才转 Frozen。
- 终态：一连接末端仅一份 Seal；迟到 Data/重复 Begin/Seal → 永久 BadState Fault；旧对象不能重开，
  恢复只通过新对象新会话（测试覆盖正向恢复）。
- `visitFrozen`：仅 Frozen；有界 `const Event*` 指针排序（不做第二份 Event 大拷贝），按 sequence 升序；
  visitor 返回 false 或每次回调后状态不再是 Frozen（重入 accept 导致 Fault）都返回 false，
  不声称导出完成（本轮修订点 3）。未触发 Seal 只含前区记录（post 无内容自然满足）。
- `Summary` 含合同全部字段：state/session/nextOrdinal/lastSequence/trigger/capacity/accepted/
  attempted/lost/evicted/retained/reason/storageBytes/wireError/storeError；
  `storageBytes=capacity*sizeof(Event)`。无 static_assert/指针位数假设；无 Windows/Vulkan/IPC/磁盘依赖。

## 测试矩阵（`AutoTest/test_recorder_history_store.cpp`）

正常：空 Summary；trigger  happy path（cut 首现于 Data、post 精确填满、全 Summary 字段、visit 内容
逐字段回读）；前环覆盖（cap=16，20 条，evicted=8/retained=12，未触发 Seal 只出前区 9..20）；
visitor 中途失败与非 Frozen 拒绝。
反向（均断言 Fault 且无部分提交）：包尾 sequence 断档（真实 Encode 产出）、包尾 kind 字节损坏、
ordinal 跳号与重放、session 错、Data 改 capacity、cut 回退、cut 变更（Data/Seal）、
固定 cut 后 Data/Seal 传 NoTrigger、Seal cut 超出已存记录、Seal accepted 与实测不符、
post 超容量、终态后迟到 Data/重复 Seal/重开 Begin、Fault 后首错保持。
wire 负例（字节修补真实编码包）：坏 magic、截断、capacity<4、记录 session 与头不符。
重入：visitor 内回调 accept → visitFrozen 返回 false 且 store 进入 Fault。
恢复：Fault 对象不重开；新对象全新会话完整走通 Begin→Data→Seal→Frozen。
容量：常规小 cap（4/16/32/64，双位可跑）+ 一例 MaxHistoryEvents=262144（约 98MiB，
storageBytes=102760448；该例若目标平台分配失败则明示 skip 而非假通过，64 位宿主门由主线程跑）。
所有正例经真实 `Encode` 产包；负例 wire 包只做字节级修补；无第二份状态机/伪造 View。

## 实际执行 / 未执行

已执行（纯 Python，`python -B` 3.13.11，cwd=集成树根）：
- 我方静态检查 `AutoTest/artifacts/recorder_offload_e1_kimi_20260916/verify_store.py` → ok=true
  （接口签名、Summary/StoreError 全字段、Decode 自持、两趟先验证后提交、防回绕锚点、
  固定 cut 两 op 覆盖、visit 复查、无禁用依赖/位数断言、括号配平；`verify_store_result.json`）。
- 既有 render-host Python 组：`run_architecture_python_checks.py --group render-host` → rc=0，
  输出在 `render_host_group.json`。
- 共享 wire 头当前 SHA `9ec432d3…b154e9`（主线程 alias 澄清版）逐字节核对未被我改动。

未执行（依合同留主线程）：任何编译器/Ninja/native 运行——`test_recorder_history_store.cpp`
需与 `recorder_history_store.cpp` + `recorder_event_wire.cpp` 一起由主线程串行编译运行
（建议 `-std=c++17`，仅标准库+线程外无依赖）；32/64 位小 cap 双跑与 64 位 maxcap 均在主线程门。
无游戏/IPC/部署/Git 写入；stdout 的 PASS/自测不代表跨进程或内存收益。

## 风险与剩余

- ordinal/sequence 回绕守卫为防御性代码路径，协议驱动的运行测试在可行时间内无法到达
  （需 2^64 量级包）；已由静态锚点与审阅覆盖，如主线程要求可加 debug 钩子再测。
- `TriggerNotCovered` 仅当 cut 在 Seal 首次给出且 codec 的 `trigger<=accepted` 放行时可达；
  测试按此精确构造（Data 不带 cut，Seal 声称更大 accepted）。
- accept 的 try/catch 把非申请类意外异常也归为 AllocationFailed；如需区分可在后续轮细化，
  本批不扩 API。
- E1 不证明 E2（真实跨进程）/E3（产品接线），不证明内存或 FPS 收益，不碰旧 Ring/渲染路径。