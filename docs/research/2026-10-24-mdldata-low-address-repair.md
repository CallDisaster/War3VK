# 2026-10-24 MDLDATA 低地址修复与真实入口验收

本记录只描述本轮源码候选和隔离验收，不把它写成 64 位渲染迁移完成，也不把
诊断构建当成玩家发布包。目标是闭合两个实际阻塞：32 位低地址碎片导致的
`MDLDATAHash` 原生分配失败，以及同一进程能否真正载图、截图并正常退出。

## 旧失败与可测根因

旧候选的原生错误是 `MDLDATAHash` 的 Storm reserve 失败，而不是 Vulkan
device lost 或地图脚本失败。错误转储的 `ProcessVmCountersStream` 给出
`PrivateUsage=1,403,662,336` bytes、`VirtualSize=2,079,582` KiB 量级；只读
低地址扫描随后实测总空闲约 13 MiB，按 64 KiB 对齐的最大连续空洞只有 36 KiB，
小于一次 256 KiB 的 native 保留请求。这个证据说明低 2 GiB 的连续地址空间是
独立约束；不能用“系统还有可提交内存”替代它，也不能把所有高地址值都当成根因。

同一旧 DLL 的 PE 静态 TLS 模板为 13,513,056 bytes/thread（约 12.887 MiB）。
其中大表属于 CurrentDraw、palette snapshot、material signature 和 geometry
ledger。把整张数组放进静态 TLS 会把地址空间压力复制到每个进入线程，即使线程从未
使用这些表。

## 本轮实现

1. `LazyThreadArray<T,N>` 保留原容量、索引、哈希、淘汰和 map-epoch 语义，冷 TLS
   只有 `unique_ptr`；首次使用才 value-initialize backing。`peek/size/capacity`
   不分配，Reset/Retire 只清已存在 backing，不唤醒冷线程。
2. CurrentDraw 的三张大表和 material signature cache 使用该组件。可选 material
   cache 的首次 backing 失败走既有 uncached signature resolver，不丢合法 caster。
3. Publish 在写入 record、occupancy、geometry ledger 或全局 publication 之前，
   按固定顺序预备三张 backing。任一分配/元素构造失败都只增加进程累计
   `backingAllocationRefusedCount`，不增加 publish attempt，不穿过 native hook，
   后续调用可重试恢复；没有 dummy table、容量削减或共享跨线程 fallback。
4. `NetEventHook` 的初始化从尚未具备 `pGameDLL/JASS instance` 的早期 bootstrap
   移到 `war3_preinit` 与 JASS instance 解析之后。`War3Events`/NetEvent 标量
   状态改为原子发布，保留既有 Present owner 的 GPU 退役和 map reset 事务。
5. 调查器的重叠消息管道读取在立即 `ERROR_MORE_DATA` 时也从
   `GetOverlappedResult` 取得完成长度。旧逻辑在本机真实消息管道中会丢掉中间
   65,536 bytes 并制造假性的重复 JSON 字段；严格 JSON 读方没有放宽。

## 证据

### CPU/离线

- lazy header 生产运行：`16,308` checks / `0` failures；涵盖异常构造、三表
  第 1/2/3 张失败、重试、warm 零分配和 reset 保留 backing。
- CurrentDraw 实际生产函数抽取：`6,489,896` checks / `0` failures，14 个函数、
  35 个 record 字段和 12,000 次独立选择对拍均通过。
- 真实 Win32 消息管道：纯合同 `37/37`，真实管道 `9/9`，20 轮累计 `180/180`；
  旧 `_transfer` 变异在合法 131,110-byte 三片消息上确实丢中段并被严格读方拒绝。
- 低地址/转储工具：`15/15` 与 `4/4`；Storm 低域 route `482,164` checks、
  `35,840` route cases、`2,240` native cases；MDL trace core `40,464`、hook ABI
  `559`、reader `62`、integration `6` 均为零失败。
- 完整配置 Meson（BelowNormal / `-j2` / restricted-Medium 非输入 Desktop）：
  `126` tests，exit 0；源码、隔离探针、构建 DLL 与玩家目录均保持不变。

### 真实隔离游戏

候选 DLL：

```text
build32_device_owner_mdl_low_20261024/src/d3d9/d3d9.dll
24,469,504 bytes
SHA-256 AA4CF7F9499A325A5849C2AA463926908F209FE3A800889ACF997EE70830BCCD
```

新的 PE TLS 区间为 `0x14CB81B8-0x14980000 = 3,375,544 bytes/thread`（约
3.22 MiB），相对旧模板下降约 75%。这只是 32 位进程的本地减量，不是 x64
迁移收益。

在新的 restricted-Medium、非输入隔离 Desktop 中，Lost Temple（SHA-256
`E305AC0B9360B36A052A27F9BA4538EDDF0C3F7EC5BB3C20D4B01416733BFFA4`）达到：

- control pipe 可用，模块身份和截图身份通过；
- MDLDATA 取证 journal 收到 152 个可信事件：19 次 22,404-byte allocation 全部在
  32 位低域，`highReturnedAddressExamples` 为 0，busy/full/read-failure 等取证计数为 0。
  这是该次启动的有界观察，不证明所有 native arena 路径、完整 free/link lifetime 或
  高地址必然是旧失败的唯一根因。
- 1902×963 截图 `AutoTest/artifacts/mdl-low-lazy-net-game-20261024-r2/game-frame.bmp`，
  SHA-256 `F6580C737B7F465C0066B9F838E71E3E726649EC880C66EC24695B641FCDB565`；
- 最后一次进程 private usage 约 663,130,112 bytes，working set 约 687,751,168
  bytes；低地址扫描 free 746,938,368 bytes，最大 64 KiB 对齐空洞 143,917,056
  bytes；
- `game.end_for_exit_test` 返回 `stockEndGameInvoked=true`，随后两次新鲜状态观察
  看到地图退场清零；仅对本次进程枚举到的唯一 Warcraft HWND 发 WM_CLOSE；原 retained
  process handle 观察到自然 `exitCode=0`。

这是“载图→截图→退图→正常退出”的本地渲染入口证据。截图不是全场景视觉等价、
不是 x64 画面交付，也不证明高压地图、跨地图长时、GPU 资源全量迁移或玩家体验。

## 未覆盖与下一门

- 未把 x64 host 接入 shipping DLL，未声明 64 位渲染迁移完成。
- 未在玩家目录部署；没有发布/提交/推送。
- 仍需多地图 A→B→A、D3D9 Reset、长时压力、完整 D3D9/WarVK pass、GPU/像素
  等价和真实玩家复测。
- `WriteAll` 的同步消息管道短写没有独立实机证据；本轮只修复并实证了客户端重叠读
  的立即 `ERROR_MORE_DATA` 丢片路径。

