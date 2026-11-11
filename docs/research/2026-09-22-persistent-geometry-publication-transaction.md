# Persistent geometry 发布事务：M3 第一切口

状态：2026-09-22 实现与本批离线验证完成；不是 GPU 或玩家验收，不是整个 M3 完成。

## 问题与唯一责任边界

`D3D9DeviceEx::War3CreateShadowPersistentGeometryAfterMiss` 原序列为：

```
geometry map.emplace → registry[key]覆写 → bytesUsed加账 → expiry.push → 返回指针
```

registry 分配失败时 geometry 已进入缓存；expiry 分配失败时前三项都已改变。
这是实际源码的异常窗口，不等于已经用玩家日志证明某次失影/崩溃由它造成。

本次共享 `PublishPersistentGeometry` 是设备和 CPU 故障注入测试共用的实际提交算法，
而不是在测试中另写一个近似模型。它只收拢元数据提交，不搬迁来源选择/GPU 创建/GC/退役。
调用仍依赖既有设备调用串行化；这不是跨线程数据库事务，也没有新加全局锁。

## 合同

- 事务基线是 **GC 与准入结束、发布开始前**，不是整个外层函数入口。GC 本来可以释放旧数据。
- 新几何插入后，用 `try_emplace` 为 registry 准备新槽；已有槽保持原值。
- expiry token 入队是最后一个可失败阶段。token 的构造/复制/移动/赋值、比较器复制与调用，
  以及最后 registry 槽赋值必须无抛出；当前 heap 为 vector-backed priority_queue。
- heap 成功后才覆写槽并加账、返回有效指针。成功路径不增加额外容器、分配或锁。
- `bad_alloc` 返回失败；仅用已取得的 iterator 删除本次新增槽/几何，不删除 ID 冲突的旧对象，
  不为回滚重新分配 registry 节点。其他异常先回滚再重抛，不把未知错误吞成成功。
- 字节加法仍由外层 `needed <= cap && used <= cap-needed` 准入保证。失败不得增加字节账。
- 容器可以保留 rehash 后的空 bucket 或 vector 容量；不承诺物理占用逐字节回到申请前。
- geometry ID 在准备上传前消费，失败不回退或复用 ID；ID 溢出问题不在本次改变。
- 已入 CS 队列的上传不可撤销。既有 lambda 的 `Rc` 和 DXVK tracking 继续保活源/目标；
  本次不等待 GPU、不提前复用存储、不新增第二套 GPU 退役机制。
- 旧 `RegistryInsert` 拒绝类别覆盖此次元数据发布失败，不把它标成 position/index GPU 分配失败。

## 验证分层

1. 先把原序列抽到共用函数，以真实 STL 容器和 allocator 失败注入得到失败回执，再修改同一函数。
2. 新 key/旧槽、node/rehash/heap 扩容、连续失败、失败后恢复、未知异常，逐字段验证逻辑状态。
3. RAII 与外部排队引用验证 CPU 强引用不提前释放；不把测试载荷当作 Vulkan/GPU 实验。
4. 静态门禁只锁设备到共用函数的接线、域/预算/来源门顺序和输出发布，不替代故障注入。
5. 固定依赖镜像、断言开启 CPU/静态回归、exact DLL 构建/no-work；玩家 DLL 保持原样。

## 独立发现、未并入本事务

- `m_war3ShadowPersistentBytesUsed` 的跨会话账需要下一独立批次：Reset 把 live map/heap
  移入 retired，后续收集却没有对应扣账。旧会话已释放后仍可能留下新会话准入占用。
  这是源码推导，尚无实际 reset/退役回归，不称为本轮玩家根因。必须先裁定 live/retired
  额度语义，再补生产生命周期测试，不能只把一个计数随意清零来绕开在途资源成本。
- GC 重排、S1 alias 多表发布、Reset 到退休队列的失败边界各自保留，不借本批重写全部生命周期。
- CurrentDraw/M4、GPU/跨地图/性能尾延迟门仍未完成；64 位继续延期。

## 验证回执

本批回执目录：`E:/WarVK-Builds/v1.22-maintenance-20260922-r2/`。所有构建父进程
BelowNormal，最多 `-j2`；使用全新 source/build 目录，不使用主树旧 build32 或 dirty 依赖。

- 原序列提取 `legacy-publication.h` SHA `319FC734C192C786890A6A679AC8F682144D23CC3761D6ABDEDB835DEEDE2073`；
  同一 `test_shadow_geometry_publication.cpp`（`5F774F3F64AB4E4098EBA73A5C5DA5CAE9A55ABAA48A24EC0910766D43B6B07E`）
  得到 **345 检查 / 106 失败 / exit 1**。原日志 `legacy-publication-test.log`
  SHA `619FEBB79E0CAC6D8E6653999420D2603AEE59D75494E5DA46D0832C6E8A9AF2` 保留。
- 修复后的同一实际算法 **345 / 0**；生产 helper SHA
  `C65CBC169E0187270CFC47249B28C4D8B36011ECCA709D5E6BCBA6E89B9DB610`。
  正常路径新 key/替换旧槽分别观察到 5/3 次容器分配，与旧算法相同；不是游戏性能测量。
- 新接线静态 **9/9**、内存字符串变异 **14/14 被拒绝**，前后源码哈希一致。
  `static-mutations/` 保存复现脚本与逐例 JSON；其职责是形状门敏感性，CPU 故障注入负责算法证明。
- 断言开启的 **102/102 Meson 测试**（92 个显式可执行目标、240 构建 edge），Meson JSON SHA
  `B78510E19047B5299F539B9A558F2B34D6B4186DE473651D1140508573C0AC66`。
- 首次全量静态 **274/276**：域触碰点与 replay binding 两个旧门仍要求 device 内直接
  `operator[]/emplace`，失败回执保留在 `static-results-review-final/`，不追认为通过。
  将两门适配为唯一事务入口、完整 domain 值交接与四流 capture→move→publish 顺序，
  没有删除用例或新增 skip；独立复核确认义务保留。重跑 **276/276 脚本 exit 0**，
  `static-results-final/summary.json` SHA `E3C3628B8AA38FA670E9D8DD5978E97202FC867AF14907E3D41A136BF81B5E5C`。
  **旧 A9/M2-5B 的 TEMP 原始快照独立重抽仍缺**；脚本退出成功不代表该子门执行，
  现有参考身份与动态 EXE 检查已跑，skip-observations 单独保留。
- 初始输入清单 `6487DCB3…D23EA5`；最终只刷新上述两份 Python 静态脚本，产品/CPU 输入不变。
  最终 `source-final.json` **3946 文件** / SHA
  `07B38D73E216ADEBAB48FCE4EE59FFBBB79217AEDA1D51CBE05D3AD44C8B7762`，全镜像逐 SHA 复核 0 差异。
  之后主树仅补三份本批文档回执，不回填或改写编译输入。
- Exact DLL 全新构建 **466/466**、两次 exact no-work、正式配置 verifier `ok=true`、
  PE `pei-i386/i386`、三个涉及 Python 门的 py_compile 和 diff 空白检查通过。
  未剥离候选 **36,157,944 B / C65CA2B2FB755AE78D5A65455A49D04577839AB896E5E70A3D043B147CFC4F33**；
  CreateNew 等价原字节冻结在 `candidate-c65ca2b2/d3d9.dll`，不是发布附件。
- 玩家 DLL SHA 仍为 `028565BCB33B6EB646E4915115B60CDA3C14C287A4225DAA50F8F6513F9E9BA3`；
  未部署、未启动游戏、未提交/推送/发布。编译/War3 进程 0；用户编辑器 PID 36012 保留，
  不把它说成全部相关进程 0。未获得新的 GPU、跨地图、画面、尾延迟或压力恢复证据。
