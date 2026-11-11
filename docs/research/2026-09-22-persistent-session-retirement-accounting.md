# Persistent 跨会话转账与退役承接（M3 第二批）

状态：实现与离线验证候选；不是同进程 A→B→A 的游戏验收。

## 已确认的源码缺口

- 原 `m_war3ShadowPersistentBytesUsed` 随创建增加、live GC 减少；Reset 搬走 geometry/registry/expiry 后没有
  搬走其账，完成序号删除 retired session 时也没有扣账。因此旧会话释放后仍可能留下准入占用。
- Reset 原先先把资源移到局部 `retired`，最后向 vector 插入；该扩容失败会销毁局部的保活引用。
  这两项是源码可证的故障路径，尚不能认定为某份玩家失影日志的完整根因。

## 单一生产交接合同

`war3_shadow_session_retirement.h` 由设备与 CPU 故障注入测试共用，收拢准备、提交和收集；
不改变地图/设备 epoch、geometry ID、domain owner-check、Arena fence 或 DXVK resource tracking。

```
创建成功 → live
Reset 承接所有 owners → live 转 retired，总收费不变
原 completion serial 达标 → 删除该 session，只减它自己的 persistentChargeBytes
```

- 收费仍为 persistent 自有 buffer 的 aligned bytes；retainSource alias 额外收费为 0，零收费依然必须保活。
- 新旧会话准入共用 `live + retired`，不扩大 cap、不在 Reset 直接清零总收费、不用减少 caster 绕过预算。
- 现有 live GC 的 erase 时减账语义不变。外部 Rc/CS/DXVK 可能仍保活 backing，所以本账不是 GPU
  实际物理驻留；退休 census 的 `cachedGpuLogicalBytes` 包含别的缓存和别名，禁止拿它扣本账。
- capacity 判定按减法逐段验证，不能因 live+retired 溢出放行；错误退休收费不应提前释放资源。

## 失败安全与所有者

- 退休队列改用稳定 list 节点。每次准备一个节点，失败不移动/复制旧 session；不是每 draw 分配。
  准备放在 map/device quarantine 之前；分配失败不推进 applied，下个 Present 可重试。
- 准备对象 RAII 撤销未提交的空节点；真正提交只包含无分配、无锁、无 GPU 调用的所有者转移与转账。
  容器 move assignment 有编译期 no-throw 校验。Rc 没有全局改 noexcept：仅向新建 null 目标移动，
  经源码核对其实现只转移指针、清 source、对 null destination 执行无动作的 decRef。
- commit 后才做 snapshot lookup/CPU alias 清理、census。该后段存在既有锁，不宣称整个 Reset noexcept；
  后段失败时已提交的退休记录仍保活，map/device applied 或 tombstone Seen 不得提前提交。
- tombstone overflow 独立 pending latch：准备前关全部 producer，成功 Reset 末尾才清；Present 在该
  latch 存在时独立重试，两个开门条件都拒绝 pending，避免 Populate 的早退形成永久关门或提前复用。
- collection 保留 `retireSerial != 0 && retireSerial <= completedSerial`。capture-off 且没有 fence 的
  UINT64_MAX 特殊策略是既有代码，本批未改，不称为新增 GPU 完成证据。

## 观察与成本

- 同一 Present 发布 total/live/retired 三个逻辑账；报告 summary 增加两个 Last 字段，workloadSeries
  尾部追加两列，旧列索引不挪动。不能与 snapshot/Arena/alias census 相加当作整机显存。
- steady-state 创建/GC 增加或减少常数个整数操作，无逐 draw 堆分配、矩阵扫描、锁或同步等待。
  Reset 每会话一个 list node，避免 vector 增长时复制旧 session；这不是实测帧时收益。
- CPU 测试直接执行生产共享事务和真实 STL/失败 allocator，检查资源保活、收费守恒、连续失败后恢复。
  静态接线与变异验证负责三入口、实际 owner 字段和 fence 交接形状，不替代 Vulkan/玩家证明。

## 验证与剩余边界

本批回执目录：`E:/WarVK-Builds/v1.22-maintenance-20260922-r3/`。
最终离线收口：

- 新 CPU 共用事务测试 **106 检查 / 0 失败**。覆盖 A→B→A 的账目模型、未完成/零 serial、
  零收费保活、分批与错序收集、连续四次准备分配失败及恢复、提交前异常回滚/提交后异常保活、
  live GC 不扣 retired、近 UINT64_MAX 准入边界。它执行生产共用组件，不是完整 D3D9 Reset 或 GPU 测试。
- 三入口生产接线 **80 检查 / 0 失败**，**12/12 内存变异被拒绝**；报告接线 6/6。
  独立审查后加强了 tombstone 清理末端才能解锁、Populate 必须在重用前关门两项守卫。
- 全新断言开启 CPU Meson **103/103**；全量静态最终 **278/278 脚本 exit 0**。
  首轮为 277/278：旧 active-ledger 测试钉住已移除的直接 move 语句，现改为验证实际 transfer
  在 commit 内、ledger clear 在 commit 后，原失败回执保留。两项 A9/M2-5B 的 TEMP 原始快照
  独立重抽仍未覆盖；本轮新编译 probe/动态差分实际运行，不能把脚本成功混同为所有子门通过。
- 以固定依赖冻结 **3951 文件**；最终 `source-review-final.json` SHA-256
  `AEABBF5BE92C6FD2ED3EABAF271A4EA507BEA1D7F65A5D47FCD81DF4CC1919D7`。
  首次清单到最终清单仅刷新两份 Python 静态脚本，产品/CPU 构建输入不变。
  镜像逐 SHA 0 差异，2910 个非归档依赖主树文件在收口文档更新前逐 SHA 一致；用户 StormBreaker dirty 未参与构建。
- BelowNormal/-j2 exact DLL 干净构建 **466/466**、exact no-work、player-release 配置校验、
  PE `pei-i386/i386`、相关 py_compile 通过。保留编译警告，不声称零警告。
- 未剥离候选 **36,125,214 B / SHA-256
  `7ECA06A72C480A4943029CDA1C6CB98B3B32AE64EB25CC964B2BACB3FF396642`**，
  CreateNew 等价冻结于 `E:/WarVK-Builds/v1.22-maintenance-20260922-r3/candidate-7eca06a7/d3d9.dll`。
  CPU JSON 回执 SHA `3CA30507B216E7B486CF1963A9850D70F111DE6D0F7ABEDB152FE98AA4E987E2`；
  静态摘要 SHA `27F61CA1C3E9B1282825C91B229FD961B831AEFF07B77F7DD2AAAE81EE2E218B`。
- 没有部署、启动游戏、提交、推送或发布；玩家 DLL 保持
  `028565BCB33B6EB646E4915115B60CDA3C14C287A4225DAA50F8F6513F9E9BA3`。
  编译/War3 进程为 0，用户编辑器 PID 36012 原样保留。

仍待：同进程实际 A→B→A、设备 Reset、压力后合法阴影持续恢复、GPU/画面/尾延迟。
Registry 的完整 domain 账户/键、GC expiry 重排异常边界、CurrentDraw/M4 和全局跨 Pass 合同仍在后续；
不把本批称为所有换图问题已修复。64 位、Water、原版模型自动灯继续延期。
