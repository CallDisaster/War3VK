# 2026-09-16 DSH（Kimi）：E2 现代 64 位宿主 helper recorder_host_main

状态：以 apply_patch 文本交付主线程审核应用（本会话无 apply_patch 工具，未写盘）；
**未编译、未运行、无双进程实测**。范围严格限于 E2 合同
`docs/plan/2026-09-16-recorder-offload-e2-contract.md`；E1 已验收不重做；不链接产品，
不启动游戏/GPU，不修改公共头/协议/旧文件/总日志。

主线程应用后补正（下文保留代理交付时设计说明，不冒充原稿已覆盖）：增加active permit的
nonce/map/device/bytes全字段及Header.session固定fixture检查；外层Decode错误真实记录到wireError；
QPC失败明确拒绝。无参数依赖初始化统一沿用旧helper exit3且不进入IPC、不输出正常host回执，
因此下文“invalid参数runtimeExit2”的原稿描述已被替代。实际native与内存证据仍待总日志收口。

## 交付

- `tools/render_host/recorder_host_main.cpp`（新增）：现代宿主 helper。
- 本文档。

## 合同逐条映射

- argc5：argv[1..3] 沿用 RH1（nonce/parentPid/parentCreation），argv[4]=normal|slow|disconnect；
  其余一律不连接、runtimeExit=2 且回执字段完整（mode="invalid"）。
  现有 `RunSlotHost` 签名要求 argc==4，helper 以 `RunSlotHost(4, argv, &consumer)` 调用，
  argv[1..3] 原样透传；若主线程改 runtime 为 argc5 直收，仅需删除这层适配。
- profile 精确 10：`capabilities()=SharedCpuSlots|RecorderEvents`；Hello 正反向精确匹配由
  RunSlotHost 既有 `HelloContract` 承担（p2/p3↔e2 均拒；此时 WVE Begin 未到，历史未分配）。
- `begin`：固定 map=11/device=17，每连接一 epoch；否则 RecorderIdentity/RecorderStore。
- `consume`（槽 payload=一个 WVE1 包，key.frame=packet ordinal）顺序：自 Decode（不信外部
  View）→ RecorderWire；`header.ordinal != key.frame` → RecorderLease；逐 Event DecodeEvent
  + `fixture::ContentMatches` → RecorderIdentity；然后 `store.accept` 自解码提交：
  WireDecode→RecorderWire、SessionMismatch→RecorderIdentity、其余→RecorderStore。
  对应场景表：wrong-ordinal=RecorderLease、wrong-session/wrong-scope=RecorderIdentity、
  bad-kind=RecorderWire、seal-totals/late-data=RecorderStore、missing-seal=RecorderMissingSeal。
- disconnect：第 3 次 Data 在写入 store 前返回 `InjectedRecorderDisconnect`，事务 failed。
- slow：首 Data 即时打印 `{"delayStart":QPC,"frequency":F}`+fflush，Sleep(1200) 后
  `{"delayEnd":QPC,"frequency":F}`+fflush；同值进入回执 delayStartQpc/delayEndQpc/qpcFrequency。
- `end()` 需 store 已 Frozen，否则 RecorderMissingSeal；`close()` 还须成功 End。
  Retire 不置 endSeen/closeSeen（沿用 RunSlotHost 语义）。
- 遍历/摘要：仅 runtimeExit==0 && endSeen && closeSeen && Frozen 才 visitFrozen；
  每条 Event 用真实 Encode 重编码、只哈希 392 字节 WVE1 Event 区（无 80 字节头、无 C++
  padding），BCrypt 流式有界批次，不产生第二份大历史。visited=0 ⇒ firstSequence/lastVisited=0、
  digest=""、contentValid=false。cpuComplete 严格 = runtimeExit0 && begin/end/closeSeen &&
  Frozen && contentValid && lost==0；lost>0 的合法 Seal 仍产出诊断 digest 但 cpuComplete=false。
- 内存：before 于 wmain 起始冷点采样；allocated 于真实 WVE1 Begin 被 accept 后采样
  （未分配时与 before 相同，不伪称分配）；active 于遍历后采样。均为主线程
  process_memory_probe 的真实采样。
- 回执：RunSlotHost 返回后打印一条 recorderHost JSON（合同全部字段，枚举为真实数字，
  trigger 未触发时为 18446744073709551615），进程返回 RunSlotHost 实际 exit。

## 主线程编译/链接提示（未改 Meson）

新 64 位可执行：`recorder_host_main.cpp` + `slot_host_runtime.cpp`、`slot_wire.cpp`、
`slot_ledger.cpp`、`shared_slots.cpp`、`win32_transport.cpp`、`win32_security.cpp`
（按 RH1 现有依赖）、`process_memory_probe.cpp`、`recorder_event_wire.cpp`、
`recorder_history_store.cpp`；需链接 bcrypt（BCrypt* API）。

## 实际验证 / 未验证

已做：只读核对全部引用符号（slot_host_runtime/slot_ledger/slot_wire/shared_slots/
process_memory_probe/recorder_event_wire/recorder_history_store/recorder_lab_fixture）
与现有头一致；内存内括号配平（{}51/51、()174/174、[]6/6）；逐场景推演 reason 映射。
未做：任何编译、native、双进程、游戏/GPU。digest 与 Gemini 独立 Python struct 的一致性、
slow 实际阻塞窗口、内存门数值均待主线程 64 位门实测。stdout 行不代表跨进程成功或内存收益。

## 风险

- 依赖 RunSlotHost 现有 argc==4 签名；主线程若改为 argc5 直收需同步去适配层。
- digest 的逐事件重编码为 O(n) 额外 CPU（262144 条 ≈ 26 万次小编码，实验室规模可接受）。
- consume 双 Decode（consumer 预检 + store 自持）有界为 160 条/包，符合 accept 自 Decode 合同。
- E2 不证明游戏内存/FPS/崩溃修复；E3 需独立合同。
