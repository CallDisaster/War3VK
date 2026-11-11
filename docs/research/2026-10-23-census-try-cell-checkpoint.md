# 2026-10-23：诊断 census 不再强制设备多线程锁

范围：作者要求先实现局部 POD、有界 try-cell、成组事务并测试，再继续调查高地址链字段。
本批不改游戏分配返回值、Game.dll 跳转、EXE/LAA、Shadow Arena 预算或 x64 Game.dll 准入。
不覆盖玩家 DLL，不提交、推送或发布。

## 实现与责任边界

- `D3D9DeviceEx` 的 `m_multithread` 始终使用原 `BehaviorFlags & D3DCREATE_MULTITHREADED`。
  删除诊断强制 TRUE、纹理入口新增的设备锁及 sink 内的设备锁；原业务锁保留。
- Draw 只交接参数 POD；四类 knownMask 为 `03/43/13/D3`。shader/FVF/texture-stage、
  非 UP 的 stream stride、非 UP 的 IB format 不再额外读取，明确 missing，不以零冒充已知。
- FF key 从调用线程当场已生成的值复制；Uber VS 生成一次局部 key，将同一值写给 upload 和
  recorder，不回读映射上传页，不把 key 采集移入 CS/GPU 工作。
- `ConcurrentCensus` 独占纯 `Census` 状态。每次只试一次 atomic_flag，失败立即返回；
  成功以 RAII release。cell 内只有有界计数、固定 32 项搜索/保存和定长副本；
  无等待/自旋重试、allocator、logger、文件或设备调用。
- Draw/Texture 的 API 与事实共用同一次准入；Present 使用同一个 API 计数，不保留第二份
  可漂移的 Present 计数。普通 noteCall 误传分组 API 会记录 unclassified，严格读方拒收。
- 序号和全部状态在 cell 内按值复制；失败不改输出。设备 ordinal 在构造发布前固定；
  冷路径的进程 ordinal 分配不在 cell/热路径内。
- `recordBusy` / `snapshotBusy` 为独立 32 位原子读数，不与 coherent copy 或彼此同瞬间。
  单次 fetch_add、无 CAS 重试；完成溢出后黏性标记令该导出计数钳制 UINT32_MAX。
  竞态读可混合时刻，因此零值也不证明无丢失。`counterSaturated` 为 0/1 标志，不是次数；
  不将这些不同单位与 accepted API 相加成总数。
- v2 导出器只接受 copy/refusals/不可变配置，序列化和 Logger 在 cell 外。首次 1/64/256/
  1024/4096/16384/65536 accepted Present 可尝试导出，正常析构再尝试一次；busy 不补排队。
  默认编译关闭，未把诊断候选当性能基线。

## 读方与真实交接

旧 v1 保留旧字段/锁口径，不伪造拒绝零值；v2 单独精确字段校验。同设备不能中途换版本。
即使有 busy，accepted 的四条恒等式仍严格成立：API 总数、Draw API/事实、Texture API/事实、
Present API/Present 次数。32 个保留样本以后的 missing 累计直方图也校验。

refusal 读数不按 snapshotSequence 强行判单调；报告 selected 与 maxObserved，并拒绝完整调用
总量认证。实际计数 `saturatedUpdates` 非零依旧不能通过算术认证。

CPU roundtrip 直接使用生产 try-cell → copy → 同一个 `WriteCensusSnapshot` → 生产 Python
解析器，使用合成参数明确覆盖空、四 Draw、三 Texture、截断及正常析构记录；不是游戏数据。
游戏 runner 也改为调用该严格解析器，不再仅凭 schema 字符串把半份 JSON 当作 usable census。

## 验证状态

- 主线程两次 fresh 定向 Python：**各 134/134**；最新回执
  `AutoTest/artifacts/native-census-try-cell-python-20261023-r2/receipt.json`。
  不是全库回归。
- 真实历史 v1 r2 日志兼容复读：4 条、device 1、sequence 4、算术通过，refusal unavailable。
- fresh x86 构建 r1 已完成 477 步并链接，但 no-work 检查要求重新生成，**该回执失败保留**。
  独立全输入 hash 核对发现 `src/d3d9/meson.build` 与既有 `war3_module_loader.cpp`
  在构建期间改变，其他捕获产品输入未变；原始清单与差异见 r1 的 `INPUT_DRIFT.md`。
  不回滚工作区、不将首轮归为通过。r2 对同一独立 build 显式 `--reuse`，重新生成后构建/
  no-work 及全输入前后身份一致通过。未使用旧 build32 或 dirty StormBreaker 子项目。
- 新 DLL：`build32_device_owner_try_cell_20261023/src/d3d9/d3d9.dll`，
  **34,566,144 B**，SHA-256
  `346B25D3B6968BA338EF88EFA267318AEDFC04BC5064B83A8BBDB5C448665D80`。
  回执 `AutoTest/artifacts/native-census-try-cell-build-20261023-r2/receipt.json`。
- x86 目标文件独立审查确认每条 hot capture 路径一次 byte xchg，拒绝一次 32 位
  `lock inc` 后返回；没有 hidden 64 位 CAS retry。Draw 两处静态 xchg 是互斥分支，
  不是每次执行两次。唯一 cmpxchg8b 重试在冷构造 ordinal，不能宣传成全部操作有界。
  copy 的 memcpy 6064 B 在 cell 内，Logger/流构造在释放后；不外推 CRT 全调用图或尾延迟。
  回执 `AutoTest/artifacts/native-try-cell-object-audit-20261023-r1/READ_ONLY_RECEIPT.md`。
- 两次 restricted-Medium 非输入 Desktop CPU gate **各 569 检查、0 失败**，三条生产 writer
  记录由正式解析器回读。r2 又固定了三条的 device/sequence/reason 与中间累计量，防止
  “空设备实际有 Reset”或“末条不再是 destruction”仍误通过；r1 不追认为覆盖这些新增判据。
  空设备 1 在正常销毁导出，设备 2 在 Present/Reset/销毁导出，不造 Present=0 的 milestone。
- CPU r2：3 写者、1 读者，共 60003 次已知 API 事务尝试，接受 21517、busy 拒绝 38486；
  接受 snapshot 115、拒绝 1176，确定的 halfway snapshot 1，随后写者继续推进，自洽失败 0。
  这些 quiescent 测试计数可精确对账，不等于生产的独立拒绝读数可与 accepted calls 相加；
  也不把此刻意争用场景当实际游戏丢失率/性能测量。
- 实测纯 Census 6064 B、wrapper **6104 B**、copy **6072 B**；测试窗口堆分配 **0**。
  这不包括 cell 外的冷日志格式化，也不声称诊断开关零成本。CPU gate 的 source、候选与
  玩家身份保全、正常 exit 0、Job/Desktop 清场及零全局输入/无桌面切换检查均通过。
  最新回执 `AutoTest/artifacts/native-census-try-cell-cpu-20261023-r2/receipt.json`。

### 真实游戏观察：两轮均未覆盖 ready/截图，保留红门

同一候选、同一 LostTemple 内容、两次全新隔离实例，restricted Medium、非输入 Desktop、
原 120 秒门不变。实际模块身份复核通过；**这不是 x64 游戏渲染**。

| 观察 | v2 记录数 | 最后 accepted Present | accepted calls | Draw attempts | 终态 |
|---|---:|---:|---:|---:|---|
| game r1 | 2 | 64 | 21391 | 1416 | ready=false、无截图、120s Job 强制清场 |
| game r2 | 4 | 1024 | 339701 | 24648 | ready=false、无截图、120s Job 强制清场 |

两份实际日志的所有记录均通过正式严格解析；`diagnosticForcedDeviceLock=false`，
`behaviorFlags=66`（未请求 D3DCREATE_MULTITHREADED），最新 recordBusy/snapshotBusy/
counterSaturated 均 0。这里的 0 不认证无丢失，也不能将 loading/menu 等未知阶段的 draw
冒充地图场景采集。当前只知道没有取得 ready/截图，不单凭 IPC 缺失断言地图物理状态。

两轮退出码 2 来自超时 Job 清理，**不是自然退出，也不是已确认 0xC0000005**。
两轮本次索引目录中只找到 crash-handler 状态文件，没有新 fatal dump；这不是没有崩溃的证明。
Job 都为空、observer joined、Desktop/creator 收口、cleanupErrors=[]、输入 Default 前后不变；
总回执仍为 `ok=false / NOT_COVERED / normalExitProven=false`，不将强制清场改成通过。
源游戏/地图、候选和玩家 DLL 前后身份保全。玩家 DLL 仍为 36,618,365 B /
`1FFE0BD14C51C8E1D2A9EB94425A06260CD4B480A7CA735E13E946EA93EE35CF`。

回执和原始日志分别位于：

- `AutoTest/artifacts/native-census-try-cell-game-20261023-r1/`
- `AutoTest/artifacts/native-census-try-cell-game-20261023-r2/`

本批可以认证移除诊断强制锁的源码/生成代码形态、并发组件合同、真实 writer/reader 交接；
不能认证游戏入口恢复、旧高地址异常修复、性能/尾延迟收益或完整迁移。此前 census-off 也有
独立 ready 超时，故不能把旧故障唯一归因于强制锁，更不能由本轮 busy=0 推导全局无竞争。
收口时再次核对 r2 捕获的 **1514 个产品输入全部未变**，DLL+两测试目标仍 no-work，
候选/玩家 SHA 不变，War3 进程数为 0。定向 `git diff --check` 通过（仅既有换行策略提示）。
独立只读审计另行重算同 1514 输入、候选/玩家与原始日志 SHA，并用生产 reader 复读
CPU 和两份真实日志；无新差异或需要撤销的局部通过项，两个游戏总门继续保持失败/未覆盖。

## 高地址调查独立推进

只读追踪现已锁定 `MDLDATAHash` 的 `0x5784` 请求、Storm 401 thunk 和实际 backlink 写点，
失败低节点名称为 `buildings\Undead\Ziggurat\UBirth`。该请求按当前尺寸策略原生透传，
不受大块 TLSF 低地址约束；不由尺寸分类认证高节点具体从 Storm arena 分配，protect-memory
模式也可能走 external VA。具体来源、四字段数值重建、未保留高页及下一探针规格见
[高地址链来源追踪](2026-10-23-high-address-link-allocation-followup.md)。

数值重建不是分配录像。尚不能认证高节点当时有效/存活或排除先前非法写；也不能将此前
census-off 的 ready 超时改名为同一访问违规。本批诊断改造不是高地址根因修复。
