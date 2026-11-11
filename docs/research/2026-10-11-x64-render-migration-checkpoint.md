# 2026-10-11：主线收口与 64 位渲染迁移 checkpoint

状态：已完成本批可验证 checkpoint，完整迁移仍开发中。作者已明确将 64 位从延期项改为立即实施；旧路线图的优先级不再覆盖本决定。
本批没有发布、提交/推送、替换玩家 DLL、启动游戏、修改系统文件或改动私有共享核心。

## 先说结论

已经开始写和运行 64 位渲染代码，不再停留在 CPU 取证外置方案。
独立实验分别证明了跨位数 GPU 图像交接、由 64 位向 32 位拥有的窗口显示，以及现有 DXVK 核心
在 64 位进程中使用原有分配器/命令队列工作的能力。
随后已有 x86 命令队列→IPC→真实 x64 DXVK 缓冲资源的接线与实跑；并非只有分别自运行的演示。
但还没有连接成游戏里的远程 D3D9 渲染器，**本批游戏侧迁出量和内存节省均未证明**。

选择的是“薄 x86 D3D9/游戏接入端 → 现有 DXVK/WarVK x64 宿主”，不是先重写一套只能绘制部分
原生对象的新引擎。完整 D3D9 语义保留地形、单位、UI、特效等覆盖；共享核心原生路径继续保留，
但目前输入仍不完整，不能拿它替代全画面链。

## 长期主线本批闭合的实际风险

`ResetAuthorState() noexcept` 旧实现会 `reserve/push_back` 一个临时对象数组。
在 32 位内存不足时，临时分配可能抛出 `bad_alloc`，从而终止进程，后续清理也到不了。
现改为锁内交换并移交旧注册表，锁外按原线程清理；不复制整个表、不新增资源退休系统。

证据：`AutoTest/artifacts/managed-reset-20261011-r1/receipt.json`。

- x86/x64 各 26 项实际组件/registry/typed transport/Weather Reset 检查通过。
- 两宽度天气协议各 211/0，既有 JAPI 协议都退出 0。
- 两份隔离源码镜像恢复旧 reserve 实现，使用字节相同测试；均在真实旧 reserve 触发
  `bad_alloc` 并被精确判为反例成立。其他非零退出、编译失败或超时不能冒充该结果。
- 只证明受测试的普通 C++ `new` 拒绝窗口。既有协议测试宏仍排除了 point/fog/Walk/Lightning/Curve
  清理；不能扩写为全部后端、真实低内存游戏崩溃或 GPU/换图验收。
- 本批尚未重建游戏 DLL；已交付的天气预览不自动包含本次修复。
- 另以原生产命令、不带协议测试宏，独立编译完整 JAPI 翻译单元成功，回执为
  `managed-reset-20261011-shipping-compile-r1/receipt.json`；实际后端引用没有被测试宏跳过。
  它仍不是链接/实机证明，原天气预览 DLL 哈希保持 `8A156518…4090CE`。

## 64 位已有的独立实际证据

### RH2：真实跨位数 GPU

64 位创建几何/图形管线并 indexed draw；32 位导入同一显卡/驱动的外部图像。
双向 GPU semaphore 和 EXTERNAL 所有权转换控制交接，CPU ACK 不作 GPU 完成证明。
读回仅为实验校验，不作为逐帧搬图的产品路径。独立 Python 全图 oracle 检查每个像素位置与颜色。

`x64-render-20261011-rh2-r4/receipt.json`：资源通道增加后对共用传输再次完整实跑，五场景通过。
正常两代际分别为 64×64、96×80，各 8 个 draw；错代际、重复帧、错 nonce 均在合法帧后精确拒绝，
错显卡为具名 AdapterMismatch。逐 epoch 的 submit/fence/exchanges 严格核对，不跨代补偿。
wire CPU 测试两位宽各 9518 检查；CPU 读方反例 26 个方法通过；四项联合 Python 回归为 79/79。

验证层没有冒充通过：host-validation-r1/r2/r3 均保留为失败。
r3 真实 host 报告 `requires VK_LAYER_KHRONOS_validation`，在 Begin 初始化阶段拒绝，并非已证明的
GPU 驱动崩溃或 5 秒超时。早版客户端在收到断管后立即关 Job，会抹掉 host 的最后诊断；现先给
精确子进程有限的故障报告机会，记录清理前真实退出状态，普通帧/共享 RH0 超时未放宽。

本机当前执行令牌为管理员，HKLM 没有该 explicit-layer 注册，单给测试进程 `VK_LAYER_PATH`
没有使层可见。这与 [Vulkan Loader 的高权限环境限制](https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderLayerInterface.md)
相符；不修改全机注册表或绕过加载器限制来把门标绿。x86/x64 validation layer 验收仍未覆盖。

### RH3：x86 窗口、x64 显示

`AutoTest/artifacts/x64-render-20261011-rh3-r2/receipt.json`：资源通道增加后再次实跑；32 位创建隐藏、不激活窗口且持续泵消息；
64 位对同一 HWND 创建 surface/swapchain，320×240 显示三帧，再 400×260 重建显示三帧，正常关闭。
x86 没有加载 `vulkan-1.dll`，没有 CPU 图片 IPC。实验分别等待 render 与 present-completion fence。

当前只覆盖这个正向隐藏窗口场景；需要 `VK_EXT_swapchain_maintenance1`。
不是玩家前台视觉/FPS、游戏 fullscreen/Reset、全部驱动或故障恢复证明。

### 实际现有 DXVK 核心 x64

`AutoTest/artifacts/x64-render-20261011-core-r1/receipt.json`：fresh 独立构建
`build64_render_host_20261011`，BelowNormal/-j2、221 个构建步骤及 no-work。
目标链接原 `dxvk_lib`，未链接 D3D9/game hooks/StormBreaker；没有改 x86 游戏结构断言。

在本机 RTX 4060 Ti 上，三个不同的 GPU fill→copy 序列经过原 Context、allocator、submission queue、
timeline fence 与 usage retirement，3072 个 uint32 全部与独立预期相符。
正常生产对象析构返回后才输出成功，进程退出 0；外部 watchdog 介入不会被判通过。
4096 B 是每个缓冲的逻辑长度，不是整台进程/驱动/allocator 的总成本。

这还不是 DXVK shader draw、完整帧、游戏 D3D9 命令接入或 WarVK passes 迁移。
RH2 的小型 Vulkan backend 仅是互操作测试，未来产品复用真实 DXVK 核心，不保留第二套自研 renderer。

### X1：有界命令队列与真实资源所有者交接

这不是继续让一个 x64 演示程序自己填充自己的缓冲区，而是 x86 生成有序命令和数据，
专用 I/O 线程传给 x64，后者调用上述同一 DXVK core 创建、更新、读回和销毁资源。
仍是独立实验室集成；**没有声称调用了 D3D9 COM API，也没有接入游戏**。

1. `render_command_queue.h`：固定 4×64 KiB，实测对象大小 262528 B；Full 不消耗 sequence、不改
   已提交字节，不授权本地事务提交。出队获得独立副本，精确 CPU ack；Stop 不等于完成，producer
   close 后才能排空到 Finished。故障不可复用旧 scope。禁止运行调用窗内普通/过对齐 C++ new。
   `x64-render-20261011-command-queue-r1/receipt.json`：两位宽各三轮实际通过；每轮两线程交接
   10000 批，另含满队列、游标/序号边界、确定性四批 Stop 排空及并发 Stop。Fault 并发尚未单独测。
2. `buffer_commands.*`：WRC1 小端头、明确资源 ID/代际/地图/设备与序号，不传指针或 C++ 闭包。
   解码→只读预检→真实后端成功→私有提交；错误不推进资源账。Destroy 留代际墓碑，Finish 要求
   所有资源已销毁。两位宽各 **2432** CPU 检查，99 B golden 由独立 Python struct 对拍。
3. `dxvk_core/buffer_backend.*`：真实设备本地资源；固定 host staging，经生产 `copyBuffer` 完成
   任意字节范围上传，而非偷偷把 wire 收紧成四字节对齐。依据
   [VkBufferCopy 合同](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdCopyBuffer.html)；
   [updateBuffer 的四字节限制](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdUpdateBuffer.html)
   不能被忽略。每次复用/销毁前分别证明 CPU submit、GPU timeline 和原 DXVK usage 退休。
4. `x64-render-20261011-buffers-r3/receipt.json`：最终源码真实四场景通过。正例 14 条命令、9 个 GPU 批次，
   6 次上传共 229505 B，3 次 GPU 读回共 360509 B，三份全缓冲 hash 与独立 Python 预期一致。
   包含超过 64 KiB 的分块上传、offset=3/length=97 更新、销毁后同 ID 第二代重建。
   旧代际/错 map/越界三反例均只接纳前 9 条，backendCalls=9、GPU 批次=6，不执行坏命令。
   x86 没有加载 Vulkan，所有子进程按精确身份退出；r1/r2 中间回执保留，不继承为 r3。

这里的 4 槽×256 KiB 是**隔离实验资源上限**，没有修改游戏预算，也不是实际 DXVK 物理驻留上限。
host 另有固定 staging 65440 B + readback 262144 B；x86 还有生产者编码区/在途批次/线程等开销。
这些数不能相减后写成魔兽已减少的内存。读回只作测试 oracle，产品不能每帧 CPU 搬图。

独立审查暴露并修正了验收读方问题：普通 int 相等会接受部分 bool/float，`expectedRejected is not normal`
会接受 null，且 DWORD PID 必须在调用 ctypes 前拒绝超宽值。原 native 回执类型正确，不能因此说原 GPU
运行失败；也不能只修脚本后继承旧源码身份。新增的实际读方反例 37 个 unittest 方法通过，
与此前 GPU 读方/JAPI/天气/跨地图定向回归合计 **116/116**；修改后已用 r3 真正重跑四个 GPU/IPC 场景。

产品级 owner 仍未完成：当前使用受外部 watchdog 监督的 lab transport，内核 I/O 最后结算可能阻塞；
本次将其放进独立测试 I/O 线程，并不证明可以直接链接游戏。背压等待属于实验调用者策略，非每帧成本基线。
下一项应按[真实 D3D9 边界](../plan/2026-10-11-x64-d3d9-migration-boundary.md)做依赖切分/COM 代理，
再让独立 x86 程序通过实际 D3D9 API 在 x64 完成第一条 draw，不继续用实验数量替代游戏迁移比例。

## 尚未完成、不能在这轮报告里消失的项目

1. 真正产品级异步 transport owner、有限上传窗口，以及不可静默丢命令的背压与故障协议。
2. D3D9 COM/资源 ID、Lock/Unlock、shader/stateblock、query/readback、Reset/Present 的跨位数接线。
3. 将 Arena/快照/几何缓存和 WarVK shader passes 的唯一所有者逐项迁到 x64，并删除对应的 x86 大副本。
4. 游戏全画面对照、压力/换图/退出、x86 VA/commit 与帧时间尾延迟的真实 A/B。
5. 天气预览仍缺前台艺术/性能和天气实际换图/Reset/resize/自然退出组合门。
6. M3 后续已识别 S1 early-cache 发布事务的异常安全审计点：旧索引/账先修改，后续 draw copy 或
   reverse-index emplace 仍可能失败。尚未修改或实测，不能借本次 Reset 修复声称它也闭合。

64 位解除的是渲染进程受 32 位 VA 的束缚，不会使原版游戏自身逻辑内存消失，也不会使 GPU 显存无限。
动态显存预算、GPU 最后使用证明与故障收口仍是必须保留的机制。

## 产物与可复现入口

- 资源宿主：`build64_render_host_20261011/tools/render_host/dxvk_core/warvk-dxvk-buffer-host.exe`。
  PE32+/AMD64，4,800,512 B，SHA-256
  `3A37A44DB3D5309A58E8363AD65AA31175312986D46AA37514E289F80FE0C1BF`。
- x86 测试端与双方精确身份：`AutoTest/artifacts/x64-render-20261011-buffers-r3/receipt.json`。
- 主入口：`AutoTest/run_render_host_buffer_gate.py`；独立验收读方：`AutoTest/test_render_host_buffer_gate.py`。
- 本批所有 gameLaunched/productIntegrated/memorySavingsProven 仍为 false。
- 最后独立 SHA 复核：buffer-r3 的720项源码、RH2-r4的24项、RH3-r2的14项、queue-r1的5项
  与当前文件全部一致；这些清单有重叠，不相加成独立模块数量。所有本批构建/测试进程已收口。
- 原天气 DLL 保全：34,467,840 B，SHA-256
  `8A156518BD029D106EC009DC3C702D0005746E2E97E4589C1C18F8F4684090CE`。

不向玩家交付这两个实验 EXE 作为渲染更新，不修改根发布日志或晋升稳定版。
