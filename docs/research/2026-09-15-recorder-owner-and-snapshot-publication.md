# DLL 取证所有者与快照页原子发布

状态：候选。与渐进式架构计划的关联是收拢会话决策、分离 GPU 所有权和 CPU 发布；不是阴影裂缝已修复声明。

## 内置调度的职责

显式 `DXVK_WAR3_FRAME_EVIDENCE=1` + `DXVK_WAR3_FRAME_HISTORY_SELF_CONTAINED=1` 时，
已有 FrameHistory 所有者持有一个低优先级线程，等待地图渲染就绪后 arm CPU/图片环。
Ctrl+Shift+C 只向代际绑定邮箱提交请求，渲染边界消费该请求，不再尝试从窗口回调获取导出锁。
图片 post 窗完成、CPU 冻结且 writer 排空后，工作线程通过既有导出器写出一份 incident。
状态策略位于 `war3_frame_recorder_session.h`，GPU allocation/copy/readback 仍在原有渲染与异步截图所有者内。

- DLL 单独完成基础原始包；Python 仅在导出后分析、转视频、整理，不是录制前提。
- 本地所有者独占会话时拒绝外部 mutation，但允许只读 status；显式 ExternalWatcher 模式保留。
- 一进程一事件、不隐式 rearm/重试/覆盖；容量不足和缺失必须显式失败。
- teardown 先请求取消，再在 registry/owner 锁外 join，然后清理所属会话。取消保留已写部分文件。
- 冻结 CPU 导出对事件指针排序，按 64 KiB 分段写入，不再复制第二份满容量 Event 数组。
  32 位 262144 指针约 1 MiB；这不等于总 recorder 内存仅 1 MiB，原环、输入池和图片显存仍有各自预算。
- schema7 的 effectiveConfiguration 是实际开关状态；旧 schema1–6 读取合同不放宽。
  incident 的 rawExportComplete 与 evidenceValidated/rootCauseReady 独立，不在游戏内伪造离线分析通过。

R1 的 pre-arm Reset 问题与 R2 的版本号错误均保留为失败。R3 在零 watcher、零 recorder control 命令下，
隔离 2560×1440 / 8 次移动俯仰捕获 104 帧、前窗 1.0087939 秒，重建 14077 条输入并正常退出恢复。
R3 唯一查看图：`AutoTest/artifacts/self_contained_recorder_runs/local_owner_r3_20260915/hud-complete.png`，
由 native 截图原字节解码转 PNG；HUD 确实显示“原始包已保存”“无需后台连接”，树木投影可见。
上部场景高亮，单图不证明全 caster 覆盖、闪烁修复或与玩家基准画质相同。未重复查看全部历史帧。

## 快照页发布的实际漏洞与修订

原 `War3AllocateStage11Snapshot` 在 vector 接纳新 page 前增加 residentBytes 和 nextId。
`make_shared`/vector 扩容的 `std::bad_alloc` 会逃逸；特别是 vector 失败会留下无对应页的容量计数。
这是可从源码证明的 CPU 元数据异常安全缺口，不是那次玩家闪退的已证明调用栈。

同一所有者现在使用 `War3PublishStage11SnapshotPage`：

1. 校验容量和 id（0/耗尽拒绝，不重新归1）。
2. 构造私有 page 并把 shared_ptr 加入所有者 vector。
3. 成功后才更新 residentBytes/nextId，再由调用者增加 create/budget 计数。

只处理 `std::bad_alloc` 和空 page；未知异常继续传播，不吞掉 DxvkError，不伪装 device-loss 恢复。
当前 Vulkan createBuffer 仍在该 CPU 发布边界外，其失败行为不被本修订重解释。
不改 384 MiB 常驻上限、16 MiB 页、32 create 门、引用回收条件、GPU fence、切图隔离或 Shader。
没有复用页中的空洞，也没有以缩小绘制人口换取通过。

新增 Win32 CPU runnable 使用实际生产 publication helper，注入 make_shared 工厂失败、vector 首次/扩容失败、
未知异常、空页、id/容量边界，检查原页和计数不变、失败对象释放、下一次正常申请可恢复。
73 项检查通过。GPU 分配失败/显存压力实机不是此 CPU 测试的覆盖范围。

## `snapshot-alloc/v1` 字段

在已有 schema7 kind12 ShadowState 中，只有申请失败且录制会话有效时写入，无成功热路径记录/磁盘输出。
key 为 device 地址、render frame、map epoch，第四项0（不冒充 device epoch 证明）。

| data 索引 | 含义 |
| --- | --- |
| 0 | 原 allocation result：1=create budget，2=resident capacity，3=allocation failure，4=invalid range |
| 1 / 2 | 原始申请 bytes / 对齐后的 bytes |
| 3 / 4 / 5 | 所有者逻辑 resident bytes / 已用 bytes / page 数 |
| 6 / 7 / 8 | 当前帧 create 数 / 常驻上限 / create 上限 |
| 9 | publication detail：0=未进入或成功，1=invalid state，2=null page，3=host allocation failure |
| 10 / 11 | 下一个 page id / 累计回收 page 数 |

这些是逻辑池指标，不是物理显存空闲值；还未标识导致失败的完整 draw/object，不能独立证明像素因果。
ring 丢失仍由现有 producerLosses 报告，禁止把缺事件当成零失败。

## 尚未闭合的边界

- R3 的 32 个 semantic 选择事件均以 CanonicalReason3（NoWorldTransform）先行拒绝。
  readiness 有检查顺序；这个首个原因不排除同时缺 palette，不能由它断言“不是 palette 问题”。
  本轮没有修改这个 gate，也没有正向 semantic 消费验收。
- 高压地图真实进入、难度选择后的压力覆盖，坏帧与快照容量/拒绝链的对应，尚待独立门。
- recorder 在 armed/exporting 中途退出仍需运行测试；现有完成后退出成功不替代它。
- 未证明当前玩家高压崩溃、镜头俯仰后阴影消失和裂缝共享根因；没有发布、覆盖玩家 DLL 或稳定 CHANGELOG 晋升。

验证编号、产物身份及更新后的结论以 [开发日志](../agent-history/DEVELOPMENT_CHANGELOG.md) 为准。
