# Stage11 预算：P1b 小账与 P2a 上传边界范围候选

状态：2026-09-19 首批实现与离线门禁通过。不是稳定修复，不授权发布或改玩家现场。

## 决策与职责

沿[预算研究](../research/2026-09-19-stage11-budget-solution-research.md)推进。
页组织/驱逐 P3 仍需真实页账户，不靠 Python 人工负载默认上线；384MiB 默认上限、
caster 准入、快照 GPU copy 与最后使用关系不改变。

本批允许独立验证的 P2a 是 **上传时同步拷贝的实际字节摘要**，不是事后猜测 Min/Num、
读取 WC 上传输出、缓存原生外部指针、开启 native GPU-skin ticket 或 GPU 同步回读。
它不需要假定现场 unknown 分布；覆盖多少以及代价多少仍必须测量。

## P1b

- `DXVK_WAR3_STAGE11_BUDGET_CENSUS=1`，默认关，独立于图像/CPU 重型取证主门。
- 唯一生产入口：Present 安全点；已知主循环线程相等才遍历。控制面读 perf 的按值副本，不访问 live 容器。
- 读取 live 容器时复用 Draw/DrawUP 的设备锁，遍历结束释放锁后才排序/发布 perf 副本，失败出口同样先解锁。
  该锁仅在 D3DCREATE_MULTITHREADED 下启用；无此标志仍依赖既有单线程 D3D9 调用合同。
  Present 本身不是互斥锁，本批不声称修复所有既有跨线程调用或允许控制面读取这些容器。
- 每地图/设备最多四个 cut：首次有页、首次压力、压力后至少两个既有 GC 周期、之后首次无新增拒绝帧。
  第一个 cut 不保证“无压力”；第四个 cut **不等于阴影恢复**，仍须看 CSM/画面。
- 上限：128 页、16384 cache entries、32768 slice references，超限显式 errors/complete=false。
  堆上固定 scratch（约 1MiB）、4 份紧凑样本；没有矩阵/顶点/索引/图像副本，没有新增 GPU 占用。
- 同页范围按端点并集去重，UV/跨 entry alias 不重复计字节。区间重叠按 touched → failed → recent → cold → retired 分区。
  touched 是“当帧尝试或访问”，**不是 required caster 或 GPU draw 证明**。
- `capacity = 各类 cache 引用范围并集 + notReferencedByCacheBytes + tail`。
  容量按 aligned slice 计，不是有效顶点大小。cache 之外 CPU/CS/GPU 可能继续引用 buffer，
  `notReferencedByCacheBytes` **不是可立即覆盖的洞**；不报告“可回收显存”。
- retired cache 可见页单列；CS-only/driver backing 不可见，固定 `physicalBackingComplete=false` / `gpuCompletionKnown=false`。
  这是本批边界，不把有限账户冒充完整物理 residency。
- unknown 原因只来自已执行分支/已有 span 判定，记录次数与潜在 full-domain position 请求字节；
  **不是成功分配/真实复制量**。不会因采样新增读取源数据、改变所有权或刷新缓存年龄。
- 报告新增 `stage11BudgetCensus`，每 cut 有明确 frame/map/device/errors；未采样输出空 samples，不能当零占用。

## P2a

- 候选门 `DXVK_WAR3_STAGE11_UPLOAD_INDEX_RANGE=1`，默认关；不是发布默认。
- 仅现有 DIP-UP 和 per-draw dynamic-system-memory IB 上传 memcpy 两处。
  每 draw ≤16KiB、每业务帧 ≤256KiB，固定 1KiB 栈 scratch，无堆分配。
- 分块读取原输入一次：同一 scratch 同时写入实际上传 slice 并计算 min/max/FNV。
  超过扫描预算、类型/范围不匹配时完整执行原 memcpy，摘要无效，保持保守快照。
- 摘要为封装值，绑定实际上传 allocation、destination identity、字节长度/类型、map、单调 upload serial。
  consumer 要求 DynamicSysmemIBO + exact full draw range，仍执行原 signed BaseVertex、stride、VB/UV 边界校验。
- serial wrap 永久回退；每次 per-draw upload 清空载荷，地图 Reset 清空旧 upload；不跨 draw 缓存外部 source 指针。
- 不改 index 字节、不省 IB copy、不换 GPU buffer/pin、不移动 barrier、不重用仍在途空间；仅让已有范围裁剪入口得到可信 min/max。
- 新摘要不能授权 persistent-package observe/consume；该路径仍必须拿到自己的真实 CPU span 和完整合同。
- GPU-authored/direct 非上传 IB 仍旧 fallback。本候选不承诺已覆盖导致大地图超限的主导来源。

## 一手依据

- [Microsoft DIP-UP](https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3ddevice9-drawindexedprimitiveup)：用户内存输入、16/32 位索引、调用返回前完成访问；对应只在现有同步上传时消费源。
- [Khronos vkCmdCopyBuffer](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdCopyBuffer.html)：buffer 范围/usage/命令作用域约束；本批保留原有 GPU copy 路径，仅减少其有证明的 VB/UV 范围。

## 验证/未过门

生产共用 CPU header 测试 + 真转换器 JSON 往返 + 解析器负例 + 生产接线静态门；
旧相关静态与 runnable、exact DLL 构建/no-work。热路径真实 p95/p99、source 覆盖、同预算完整阴影、
低视角往返/长时/视觉仍未验证。即使 helper 离线等价，也不能把旧玩家失败追认为通过。

最终身份与实测回执由开发日志 checkpoint 记录；不得根据本实施文档直接部署。

## 本批回执与剩余门

- exact DLL Below Normal / `-j2` 构建完成；最终设备锁修订为 `d3d9_device.cpp.obj + DLL link` 两个 edge。
  exact target dry-run 为 no work。PE32/i386，36,386,190 B，SHA-256
  `68176AE78808CA2329EB9513502D805426401FC91101134E11C71C104F3C5378`。
- Meson 89/89（显式构建 80 个测试 executable 目标后 `--no-rebuild -j2`，含两个新增生产 header 测试）。
  Census 3,534 断言；上传摘要 90,032 断言（10,000 组 16/32 位随机对拍与身份/预算/回绕拒绝）。
  这不是完整 D3D9/Vulkan 调用的 GPU 等价证明。
- 全量 root `test_*_static.py` 首轮 266/267：旧 registry-domain 导出守卫把 perf env 名称总量冻结在 108，
  被本批两个独立开关触发。只将精确基数改为 110，并分别要求两个指定新名称恰好出现一次；
  registry 本身禁加 env 的断言保留。全量重跑 267/267，不追认首轮为通过。
- 真 census JSON writer → 严格 reader 5 例；上传接线 3 例（均包含在上述静态脚本）。
  另跑 canonical-ready producer 片段 4/4、数据采集报告 reader 6/6、独立页政策模型 15/15；
  4 个相关 Python 文件 py_compile 和全树 git diff --check 通过。静态脚本数不是全部 Python 用例数。
- Win32 CPU 测量一次回执：固定 scratch 1,065,144 B，4-cut History 66,248 B；32768 个重叠引用样本
  2,559 µs（只含 Collector，不含 live 遍历/设备锁等待/perf 发布）。
  16KiB × 10,000 的 Copy+摘要为 175,781 µs，普通 memcpy 为 4,717 µs；两者是宿主机缓存内存测量，
  防止编译器消除结果，但不是 WC 实况、游戏线程 p95/p99 或显存收益。开启有真实 CPU 成本，不能称免费优化。
  关闭时不执行摘要扫描/计数，保留原 memcpy，但仍有门分支和 per-draw 小型值字段清零/存储，不能称机器指令零开销。
- 无新 GPU buffer/readback、无预算提高、无缩减 caster、无回收/整页布局/fence 修改。
  无部署、无游戏、无 commit/merge；玩家原始报告保持不变，根稳定 CHANGELOG 不更新。

### 后续一次受控对照所需条件（尚未执行）

1. 同候选、同地图、2560×1440 非交互隔离、重型帧/图像取证关；轻量 census 两轮均开，
   `DXVK_WAR3_STAGE11_UPLOAD_INDEX_RANGE` 仅 OFF/ON 一项改变。执行前另核精确现场/候选/地图身份与恢复约定；
   当前文档不构成部署许可。上轮地图 ready 失败应先查启动链，不把进菜单当有效压力运行。
2. 必须观察 `uploadRangeHits`、unknown 分桶、完整页账户，以及旧有复制字节/容量拒绝/required omission/
   producer incomplete/CSM render serial/receiver/完整帧与尾延迟；hit 不等于成功捕获，第四 cut 不等于画面恢复。
3. 若主导 unknown 路径不属于这两个上传点，P2a 不能解决其放大；再沿实际源补范围证明，不能一键开启 GPU 回读。
4. 根据 P1b 真实账户决定 P3 混页/寿命分组或驱逐策略；cache 无引用字节不能直接回填复用，
   必须继续证明实际 CS/GPU 最后使用。P3 本批未实施，低视角往返后阴影恢复门仍阻塞合并与发布。
