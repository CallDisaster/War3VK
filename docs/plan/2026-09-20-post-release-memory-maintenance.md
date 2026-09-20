# v1.22 发布后内存维护

> 本文下方为首批历史记录。后续同寿命钉页已接入退役切片复用与自适应预算并获得单轮玩家正向反馈；当前发布范围以[1.22.01](../RELEASE_1.22.01.md)和开发日志最新回执为准。跨attempt诊断与慢I/O仍未闭合。

本轮修复独立审查的具体反例，不扩功能/caster 准入/384 MiB 快照池/1152 MiB Arena 预算，
不部署、不自动提交或重发 Release。StormBreaker 既有变化保留，构建依赖使用已提交 gitlink。

1. Image：未转移 VkImage 作用域清理，普通/导入图像完成初始化才登记；空 allocation 明确失败，真实驱动异常不吞掉。
2. CPU chunk：保留所有非零尾段；零申请与 uint32 对齐溢出在创建 chunk 前拒绝，不扩大 payload。
3. 异步截图：分离请求意图与资源就绪；按需准备，GPU/编码完成后由 Present 所有者回收空闲槽。
4. palette 诊断（下一批，尚未实施）：不同 attempt 的局部阶段不能拼接成完整恢复，生产与解析器一起验证。
5. 同寿命钉页（未解决，本批只补负例）：分组不保证同类切片同时死亡。无 GPU last-use 证明不做页内复用。

## 一手约束

- [Vulkan vkDestroyImage](https://docs.vulkan.org/refpages/latest/refpages/source/vkDestroyImage.html)：
  已提交引用须完成才可销毁。guard 仅管理尚未发布的本地句柄；allocation 接管后沿用原生命周期。
  导入外部 image 不新增所有权。
- [Microsoft Canceling Pending I/O Operations](https://learn.microsoft.com/en-us/windows/win32/fileio/canceling-pending-i-o-operations)：
  取消是请求，不保证驱动响应；完成前不可释放缓冲/OVERLAPPED。不 detach 后销毁 worker 对象，
  不把 stop 标志称为有界磁盘取消；慢 I/O 未闭合边界如实保留。

## 验证

用实际生产组件或原文函数负例，不只检查字符串。区分 Vulkan 替身、CPU Win32、GPU 与玩家运行。
新独立构建目录、BelowNormal/-j2；登记输入/DLL 身份及相关测试。旧 93/273 不追认新改动。

## 首批结果与剩余项

- **最终 source-revision3：断言开启 CPU Meson 98/98、静态脚本 274/274、py_compile 与 diff 空白通过。**
  以下 source-revision2 数字是历史 checkpoint，最终没有沿用旧结果代替重跑。
- source-revision2 的 CPU Meson 97/97、静态脚本 274/274 已通过。Image 原文函数故障注入
  70 检查、Win32 实际 allocator 139 检查；截图原文函数 56 检查。后续新增 skipped Present
  回收与截图 core 标准测试，须重新跑门禁，不沿用这些次数。
- 整页回收负例现在使用生产分配规划器（持有者/GC 是明确的 CPU 模型）：24 个同属 ShortLived
  的 256 B 锚点仅 6144 B，却使 384 MiB 全池无法接受下一次 512 KiB。测试明确打印 UNRESOLVED；
  不代表当前玩家现场已复现。释放全部锚点后才能回收，与“视角移走”不是同一个条件。
- 截图的零值指冷启动且从未请求时的 readback buffer 分配次数/逻辑容量，不包括 worker 栈、
  Vulkan 内部开销。完成后清的是槽的 Rc；CS/命令表仍可持有 Rc，DXVK allocator 也可能保留可复用
  backing chunk，因此不能承诺 private bytes / VA / 显存数字立即下降相同字节数。
- 仍未修复：跨 attempt 的 Recovered、磁盘 I/O 取消。后者不能只用超时 detach 来掩盖：
  应以可取消 I/O + 完成确认保护缓冲，并对拒绝取消的驱动保留明确关闭边界。
- 候选不等于 stable。下一物理门须同时验证首次原生截图、连续截图、history export、resize/退出，
  核对选中帧/真实 fence/尺寸/完整文件及资源曲线；不以本轮 CPU 替身冒充该门。

## 最终离线回执（未部署、未发布）

- 工作树 HEAD：`24140e4df4758e143aef2fb022af442f1b59dbb4` 加本批未提交修改。
- 构建根：`E:/WarVK-Builds/v1.22-maintenance-20260920-r1`。
  `source-revision3.json` 记录源码及已提交依赖 gitlink；14 个当前变更源码/测试路径逐 SHA 相符。
  本节和开发日志为构建后的文档回执，不回写该源码快照。
- 构建：首次 exact DLL 466/466，最终截图增量重生成/编译/链接完成；exact target no-work。
  产品配置 player-release 审计 `ok=true`；测试配置独立开启断言。BelowNormal/-j2 的过程偏差
  已在开发日志保留（一次 Meson 自动 Ninja 超并行后停止），不描述为全程无偏差。
- DLL（未剥离）：`build32-product/src/d3d9/d3d9.dll`，PE32/pei-i386/i386，
  **36,051,519 bytes / SHA-256 76500A3E75BF9764D059ED7CD9EF5E4DBF5FD7F05DEE545907FB3E44EC74590C**。
  版本资源仍为 1.22.00，身份是本地维护候选，**不是已发布 DLL**。
- 完整运行日志：`build32-tests/meson-logs/testlog.json`，98/98；最终静态日志 `static-r2/`，274/274。
  重点为 Image 原文函数 70 检查、实际 Win32 allocator 139、截图原文函数 62、截图 core 100000
  次并发交接、grouped-allocation 3224 检查（其中包含未解决钉页负例）。
- 最终截图空闲回收位于现有设备锁内的 `beginPresent()`，即使后续 Present 被跳过或格式不支持，
  已完成 Retired 槽也能归还自身 Rc；Submitted/Quarantined 保持原所有权约束。真实 GPU/截图文件
  及慢盘退出行为未在本轮验证。
- 玩家现场 `E:/Work/Warcraft III/d3d9.dll` 保持 **31,213,491 bytes /
  62BF9F402C90DE8C284F5C9C194517EC165F208F0A03F33A75D2F1FDB56381C3**；相关游戏/编辑器/编译进程 0。
  未部署、未提交/推送、未改 Release 或根稳定 CHANGELOG，编译资源已释放。

下一批优先修诊断跨 attempt 拼链并补生产往返反例；池内钉页需要独立存活区间/持有者和 GPU
last-use 证据，不能通过提高预算或抢回仍在用的页收口；慢 I/O 取消也必须独立验证完成语义。
