# 64 位 D3D9 渲染迁移：生产边界与第一条真实 draw

日期：2026-10-11。状态：源码审计与实施设计，**不是游戏已迁移或内存收益验收**。

实施追加：作者批准后，D0/D1 的局部共享、受限 COM 代理及 D2 离屏绘制已经实际执行；
Present/完整兼容仍未完成。旧单进程参考图像通过、退出 15 refs 未通过，不能标全绿。
最新源码/CPU/GPU与未覆盖项见[第一条真实绘制回执](../research/2026-10-11-x64-d3d9-first-draw.md)。
以下“只新增本文件”及“下一步”措辞描述初版设计时点，不覆盖上述后续实施。

审计基线：`codex/v1.22-main-tree-20260920`，HEAD
`a2054a9776e40edc3f01f6f390c06037965b8ef2` 加当时工作树；下列行号对应本次读取的源码。
工作树存在并行未提交改动，HEAD 不能独自代表候选身份。实施/验收时另行冻结实际源码、工具链与产物。
本任务只新增本文件，没有编译、运行游戏、部署、提交或发布。

用户已明确要求立即推进 64 位渲染，历史“v1.22 暂不做 64 位”不再作为开发阻挡。
执行仍遵守[渐进式架构收敛计划](2026-09-15-incremental-architecture-convergence.md)：
复用现有 DXVK、实际输入与来源一起交接、不同寿命分别证明、合法绘制不能因迁移而静默消失。

## 1. 决策摘要

目标是 **x86 游戏内保留 D3D9 兼容代理与游戏事实采集，x64 宿主拥有实际 DXVK/Vulkan 渲染**。
不是在游戏进程里再增加一个 Vulkan device，也不是重写一套只认识三角形的新渲染器。

下一项实施交付必须是独立 32 位程序经真实 `Direct3DCreate9/CreateDevice`、资源与状态 API、
`DrawIndexedPrimitive` 到 64 位现有 DXVK 的端到端链路。RH2 的 GPU 实验、RH3 的窗口实验、
DXVK core 的填充/拷贝 bootstrap 都是可复用前提，不能代替这条 D3D9 链路。

三项不能混淆的边界：

1. 渲染常驻历史、GPU 资源管理、shader/pipeline 编译和 WarVK 渲染 pass 可以迁移；游戏自身对象与
   D3D9 必须暴露给调用方的 CPU 地址不能全部消失。不能承诺 x86 渲染相关占用归零。
2. CPU 传输 ACK 只允许复用传输槽；它不是 GPU 完成，不允许提前回收宿主资源。
3. 诊断队列满时允许计数并丢样本；正式命令队列满时必须背压或明确失败，不能成功返回后少画对象。

## 2. 所有权划分

```text
Warcraft III / x86
  Game.dll、JASS、Hook、HWND/消息线程
  D3D9 COM 代理、CPU 可访问 backing、受限状态镜像
           │ 有类型 ID + 顺序命令 + 按值拥有字节；不传 C++ 指针
           ▼
  有界命令/上传窗口 ── 专用 I/O owner ── 有序接收与资源表
                                                │
                                                ▼
                                      x64 / 现有 DXVK
                         D3D9 共享语义、DXSO/FF、DxvkContext
                         GPU 资源/缓存/退休、WarVK pass、Presenter
                                                │
                                                ▼
                                     同一 GPU / 游戏原 HWND
```

| 事实或资源 | x86 留存 | x64 目标所有者 | 不可偷换的含义 |
| --- | --- | --- | --- |
| Game.dll 对象、句柄、动画、原生 Hook/JASS | 采集与游戏 API 执行 | 只收有来源/代际/空间的值 | 不能在 x64 解引用游戏地址 |
| COM 身份、`QueryInterface/AddRef/Release/GetDevice/GetContainer`、私有数据 | 本地代理及关系 | 有类型资源 ID 的对应实体 | 不跨进程传 `IUnknown*` 或原 vtable |
| `Lock` 返回地址、SYSTEMMEM/SCRATCH、必要 managed backing | 可访问且满足既有兼容寿命 | 远端 GPU 副本与上传历史 | “数据已上传”不等于允许撤销游戏仍可用的 CPU 映射 |
| D3D9 状态/描述符/声明/shader 原字节 | 足以实现同步 Get、stateblock 与引用的镜像 | 相同顺序的执行状态、转换与编译缓存 | 镜像必须来自同一次已接纳调用 |
| Vulkan instance/device、allocator、CS/pipeline workers | 完整迁移目标下不创建 | 现有 DXVK | 不是两进程各跑一套完整后端 |
| Arena、快照页、上传切片、完整历史 | 只留有界入口及必要兼容 backing | 实际消费者及 GPU 退休 | 不将整个 x64 缓存重新映射进 x86 VA |
| 窗口、输入、WndProc、游戏消息泵 | 原线程所有 | 可拥有该 HWND 的 surface/swapchain | 不把窗口线程或游戏回调任意搬到宿主 |
| 截图、query、readback | 调用和最终返回区 | 执行、等待和有界结果 | 宿主收到命令不代表图像/查询可读 |

仍应分别统计 x86 private/commit/VA、CPU 映射窗口与兼容 backing、x64 private/commit、
共享物理页和 GPU residency。x64 解决地址空间约束，不增加物理 RAM/VRAM；重复映射不能重复算成收益。

## 3. 实际拦截点与复用入口

这里列的是现有实现，不声称已存在下表中的跨进程代理。路径均相对仓库根目录。

| 边界 | 当前源码锚点 | 迁移处理 |
| --- | --- | --- |
| DLL 工厂 | `src/d3d9/d3d9_main.cpp:24` `CreateD3D9`；`:195`/`:206` `Direct3DCreate9/Ex` | x86 创建真正 COM 代理；能力协商后再建立宿主 device |
| interface/caps | `src/d3d9/d3d9_interface.h:18` `D3D9InterfaceEx`；`.cpp:56` 构造；`:178` 起 adapter/format/caps；`:405` `CreateDeviceEx` | adapter/caps 由实际宿主后端提供并有界缓存；保持本地错误参数/输出约定 |
| 创建实际 device | `src/d3d9/d3d9_interface.cpp:455` 起 `createDevice → D3D9DeviceEx → InitialReset`；`:489` `War3Hook::InstallHooks` | 切开 GPU 创建与 x86 游戏 Hook，不能让宿主加载 Game.dll 结构逻辑 |
| device/命令流 | `src/d3d9/d3d9_device.h:394` `D3D9DeviceEx`；`.cpp:8738` 构造；`.h:1368` `EmitCs` | 抽出共享 D3D9 语义与窄后端接口；现有 `DxvkCsThread/DxvkContext` 留在宿主 |
| texture/surface/volume | `d3d9_device.cpp:14394/14468/14532` CreateTexture/Volume/Cube；`:14693/14702/15380` RT/DS/offscreen；`d3d9_texture.h:130/163/189`；`d3d9_common_texture.h:75` | 本地 COM 描述符、子资源关系与 lock backing；宿主对应 image/view/上传 |
| VB/IB | `d3d9_device.cpp:14596/14645`；`d3d9_buffer.h:56/76`；`d3d9_common_buffer.cpp:85/101` | 固定宽度类型 ID、版本化内容、范围与真实 slice 寿命 |
| 更新/拷贝/读回 | `d3d9_device.cpp:14711/14791/14893/14957/14970/15247` UpdateSurface/Texture、RT/FrontBufferData、StretchRect、ColorFill | 对各格式、pitch、子矩形、方向分别编码；读回不能用上传 ACK 冒充 |
| scene/状态/绑定 | `d3d9_device.cpp:15387/15545/15605/15619/15654` RT/DS/Begin/End/Clear；`:15821/15856/16143/16746/16775/16871` transform/viewport/RS/TSS/sampler/scissor | 本地验证与远端执行共用语义，按调用序列更新；禁止只发送 draw 而遗漏状态 |
| 四种 draw | `d3d9_device.cpp:16936/31669/32461/32563` DrawPrimitive/Indexed/UP/IndexedUP；`:32723` ProcessVertices | 编码真实参数及被引用资源版本；UP 在返回前拥有调用方字节 |
| declaration/stream/index | `d3d9_device.cpp:32866/32949/33184/33270/33323` declaration/FVF/stream/frequency/indices | 顶点布局、偏移、索引域、实例频率必须一起移动 |
| shader/constants | `d3d9_device.cpp:32979/33394` VS/PS；`:33118/33148/33166` 与 `:33565/33583/33601` 常量 F/I/B；`d3d9_shader.cpp:94` | 原始 token、阶段、常量原位值交给相同 DXSO/FF 实现，不换写渲染算法 |
| COM/stateblock | `d3d9_device_child.h:16`；`d3d9_resource.h:20/45/67`；`d3d9_stateblock.cpp:48/64` Capture/Apply | 本地 COM 生命周期与 stateblock 引用保留；Apply 产生有序远端状态事务 |
| query/HRESULT | `d3d9_device.cpp:33649` CreateQuery；`d3d9_query.cpp:97/129/164` Issue/GetData/GetQueryData | 分离命令接纳、CPU 执行、GPU 完成与返回状态 |
| swapchain/Present | `d3d9_device.cpp:14186/33771`；`d3d9_swapchain.cpp:324/1124/1355` Present/Reset/PresentImage | 复用 DXVK Presenter/帧延迟控制，不把 RH3 的直接 Vulkan lab 当成产品接线 |
| Reset/终态 | `d3d9_device.cpp:13985/14219/34624/39866` cooperative level/Reset/ResetEx/InitialReset | 不同重置分支、默认池约束和终态 fail-stop 均保留 |

当前 `D3D9InterfaceEx` 构造已调用 `AcquireD3D9Instance()`；device 构造创建 shader/FF cache、
staging 和 `DxvkCsThread`（`d3d9_device.cpp:8744-8754`）。因此仅改 DLL loader、增加 x64 宏，
或者把同一大 device 再包一层 RPC，不会自然减少 x86 驱动/缓存负载。

`EmitCs` 的闭包捕获 `this`、`Rc`、映射地址等进程内对象。**不能序列化现有 lambda 或整个 C++ struct**；
必须先有固定宽度、范围校验、可版本化的命令，再由宿主重建本进程调用与引用。

以上表不是完整兼容清单。还须逐项覆盖 material/light、clip plane、软件 VP、mip/LOD/dirty、
所有 texture stage/sampler、额外 swapchain、格式/多重采样与 Ex 分支。
第一条 draw 的受限开发 profile 可以明确拒绝未支持 API；它不能伪装成可给 Warcraft 使用的完整 D3D9。

## 4. 必须冻结的行为合同

### 4.1 Lock/Unlock 与调用方地址

现有入口：`d3d9_device.cpp:35020` `WaitForResource`、`:35206/35498` Lock/UnlockImage、
`:35757/36091` Lock/UnlockBuffer。不要用统一“Unlock 就传一整块”替代这些实际行为：

- `DISCARD` 可以更换物理 slice，但旧 draw 仍可读旧版本；新旧内容版本及 GPU 最后使用分开。
- `NOOVERWRITE` 有不等待的已有映射路径，不能加每次全资源读回或同步 RPC。
- READONLY、DONOTWAIT 与 pool/usage 的处理在 texture/buffer 中不同；当前代码还保留忽略某些 flag
  组合、部分 lock 禁止 discard 等兼容逻辑。先用同一实现/对照测试固定这些行为，再迁移。
- 按 block-format、mip、face、row/slice pitch、子矩形与 dirty range 传真实范围；所有乘加与长度做溢出校验。
- GPU 可写资源的可读 Lock、`GetRenderTargetData` 必须完成相应 GPU→CPU 依赖再让游戏读；
  等待仍服从原本 API 的非阻塞/错误语义，不把 CPU 接收序号当完成。
- `D3D9Texture2D::AddDirtyRect` 明确保留“Unlock 后继续写旧指针、靠 AddDirtyRect 通知”的兼容路径
  （`d3d9_texture.cpp:93-111`，volume/cube 同类路径）。因此不得全局在 Unlock 后撤映射/释放 backing。
- `d3d9_surface.cpp:213/254` GetDC/ReleaseDC 还需要本地 GDI 对象及同步内容；不能返回宿主 HDC 的普通指针。

`DrawPrimitiveUP/DrawIndexedPrimitiveUP` 在 API 返回后不能再读调用方临时数组；在接纳前完成有界拥有拷贝，
并保持现有调用后 stream/index 状态效果（`d3d9_device.cpp:32516` 起及对应 DIPUP 路径）。
大于单槽的合法上传走有总长度/偏移/版本的分片提交；完整提交前不得发布半份新资源给 draw。

managed 与 SYSTEMMEM/SCRATCH 的 CPU 含义也受 D3D9 本身约束；不能简单把它们视为“全部显存数据”。
参考 [Microsoft D3DPOOL](https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dpool)。

### 4.2 同步返回、状态镜像与 shader

能在本地决定的参数校验、COM Get、状态 Get 可本地完成。需要实际宿主分配结果的 Create 必须获得真实
HRESULT 才暴露成功对象；纯本地 SYSTEMMEM 等资源则按其实际所有者完成创建，不强行每次远程分配。
禁止先返回假 `S_OK`，再在宿主资源失败时静默跳过后续 draw。

有序命令使用至少 `connection + device + commandSequence`；涉及资源时附 `type/id/generation`，
涉及 WarVK 语义事实时另附 map/frame/provenance。接纳容量、局部镜像提交与成功返回是一份事务：
不能发生本地 Get 已看到新状态，但唯一远端命令因队列满而消失。
这不是要求每个 Set/Draw 等一次 RPC；普通命令按批有序传输，真正同步 API 才等待其契约要求的结果。

`HRESULT` 保留 32 位原始位模式；枚举、BOOL、整数与浮点按已定义宽度/位模式编码，不 memcpy ABI struct。
`Query::GetData` 的 S_FALSE、FLUSH、null/size、缓存与失效行为以当前实现为准；
`D3DGETDATA_FLUSH` 不是“立即变为完成”。readback 与 query 只能从宿主真实完成状态返回。

shader 复用 `d3d9_shader.cpp:94` 的阶段/版本分析、`:60` 的 `DxsoModule::compile`、
`src/dxso/dxso_module.cpp:24` 的转换及原缓存策略。x86 保留 `GetFunction` 所需原字节
（`d3d9_shader.h:117`）；传输有界、明确长度的原 token，不只传 shader hash/presence。
原 shader 指针 API 没有显式长度，长度取得与 token 检查也须有独立正反例，不能任意扫描后跨进程信任。

固定功能不是“没有 shader 所以无需迁移”：FVF、TSS、transform、material/light 等必须驱动现有
`D3D9FFShaderModuleSet`（`d3d9_fixed_function.cpp:3001/3008/3025`）。
第一条可编程测试通过不能替代 Warcraft 的固定功能覆盖；禁止用实验内置 GLSL 替代游戏原始输入。

### 4.3 COM、Reset、地图和 GPU 退休

本地 `GetDevice/GetContainer/QueryInterface` 的身份与引用关系不能变成每次新代理；
surface 对 base texture 的 private-ref 转发（`d3d9_surface.cpp:52/66`）、
stateblock/绑定持有的引用、`D3DSPD_IUNKNOWN` 的本地引用必须保留。
最后一个外部 Release 不等于远端资源立即可销毁：还需排空被接纳命令引用，并由现有 DXVK 跟踪 GPU 使用。
句柄代际不复用/检查不能被“地址刚好相同”替代。

明确区分 connection nonce、D3D device、D3D Reset、map epoch、内容版本和 GPU completion serial。
通用 D3D9 资源可能合法跨图存在，**不能把所有 D3D9 资源都绑到 map epoch 后一刀切销毁**；
地图身份只约束实际 map-scoped 的 WarVK 事实/缓存，设备及应用拥有资源按各自合同处理。

`Reset` 的 outstanding default-resource 拒绝、backbuffer/state 变化及 `ResetEx` 的差异不能统一省略。
终态 Vulkan device loss 继续 fail-stop，不把恢复连接当成原 GPU device 自动复活。
WarVK 当前 Present 安全点仍是唯一地图/设备转换决策所有者：
`d3d9_device.cpp:13440` `War3QuarantineShadowSessionAtPresent`、
`:13473` `War3ApplyShadowMapEpochResetAtPresent`、
`:13520` `War3ApplyShadowDeviceEpochTransitionAtPresent`。
迁移这条有序边界，不并行建立由 IPC ACK 自行释放 Arena 的第二套生命周期。

### 4.4 窗口、消息泵与故障

游戏 HWND、输入和消息线程留在 x86；x64 通过明确验证的窗口所有者/进程身份建立实际 DXVK Presenter。
HWND 是特定可跨位宽传递的 OS 对象标识，不代表普通指针可跨进程；HINSTANCE 的取得也必须符合目标窗口要求。
参考 [Microsoft 32/64 位 IPC](https://learn.microsoft.com/en-us/windows/win32/winprog64/interprocess-communication)
及 [VkWin32SurfaceCreateInfoKHR](https://docs.vulkan.org/refpages/latest/refpages/source/VkWin32SurfaceCreateInfoKHR.html)。

WSI create/destroy/acquire/present 可能触发向窗口线程的同步消息；x86 若阻塞在等待宿主且不处理所需消息，
可能死锁。必须设计可验证的窗口 owner rendezvous，并保持消息泵活跃，不能在主线程持有需要被回调的锁时等待。
也不能为了等待而任意重入游戏所有消息。具体重入/串行边界需用真实 DXVK WSI 验证。
参考 [Vulkan Win32 surface 的消息泵约束](https://docs.vulkan.org/refpages/latest/refpages/source/vkCreateWin32SurfaceKHR.html)。

连接丢失、超时、对端退出、协议错误、GPU device loss分别记录；初始化时限、日常背压、GPU等待也分开。
对未确定宿主是否执行的批次禁止盲目重发并重复执行 draw。恢复需新 connection 与明确资源重建，
在其证明完成前报告不可用，不伪装自动恢复。

## 5. RH1 可以复用什么，不能直接接进游戏什么

| 现有能力/源码 | 可复用内容 | 当前限制 |
| --- | --- | --- |
| `tools/render_host/producer_inbox.h:39/53` | 固定容量、本地 SPSC、按值发布/取出、关闭协议 | `:71-74` Full 直接计丢失；只适合现有诊断样本，不是无损 render queue |
| `slot_ledger.h:9-12`，`shared_slots.cpp:78/122/136` | 4×64 KiB CPU 槽、nonce/代际/范围、有权读写与私有副本 | 传输槽容量不是整帧/整资源最大值；ACK 不拥有 GPU 寿命；映射/互斥/哈希不进逐 draw 热入口 |
| `slot_host_runtime.cpp:38` | 单宿主 owner、有序处理与固定宽度协议 | 新 render capability/version 明确分开，不把诊断 payload 偷换成 draw |
| `AutoTest/test_render_host_sample_worker.cpp:15/96` | 专用 I/O worker 示例、对端身份、退出与故障证据 | 明示 lab-only；`:24/38` thread join 为 INFINITE；单在途 request/ACK 不是吞吐完成方案 |
| `owned_child.cpp:11/77`；`win32_transport.cpp:16/29` | 精确 PID/creation、owned Job、对端故障、取消后保活 | destructor 与取消 drain 有真实阻塞等待；外层实验 watchdog 不等于游戏可安全析构 |

因此，现有异步 sample worker **尚不是可直接调用的游戏服务**。产品化需要专门 owner 状态机，
不在游戏/render/DllMain 回调中 join worker、等待 child 或销毁仍被内核引用的 OVERLAPPED 容器。
“requestStop 已设置”不能替代线程/内核工作已结算；若不能安全结算，按既有终态政策保活/隔离并报告，
不得为按时退出制造 UAF。实验工具仍可保留其外部 watchdog 失败边界。

正式命令背压合同：

1. 先为命令和其必要 payload 预留有界额度，再提交本地可见状态/返回成功。
2. 所有已接纳命令严格有序且保留所有权；满时等待允许的额度释放或按该 API 的真实失败语义处理。
3. 禁止无界 spill queue、丢 draw/丢 caster、丢整帧后返回正常，或通过增预算掩盖错误。
4. 不承诺“有限内存 + 永不阻塞 + 下游任意慢 + 永不丢失”同时成立；记录实际阻塞位置/耗时与长尾。
5. 保留独立字节/命令序号和断线接纳边界。只有宿主复制/消费结束才复用 CPU 槽；
   宿主 resource/slice 另由 DXVK GPU-last-use 退休。
6. SPSC 只在生产者确实被序列化时适用。现有 `LockDevice` 与 `D3DCREATE_MULTITHREADED`
   （`d3d9_device.h:1180`，`.cpp:8748`）仍需保持；不能把单线程实验推成任意 API 并发安全。

## 6. 下一条实际实现链：不是继续重复 RH2/RH3

### D0：先切开真正的编译依赖

`src/d3d9/meson.build:97` 起直接含 JASS/Hook/native 等；
`war3/native/war3_native_renderer.h:418/1564` 起及 `jass/war3_game_struct.h:2759` 起有 x86 布局约束。
必须将 Game.dll 指针读取/Hook 与可跨位宽的 D3D9/DXVK 语义分出明确边界。
不能关闭 static_assert、把游戏指针扩成 64 位，或散布无行为 stub 来声称 host 已支持游戏。

优先让 shipping 和 host 消费**同一份**抽出的 D3D9 校验/状态/资源执行逻辑；
宿主 decoder 只负责 wire 到这些入口的转换。已有 shader wrapper 接收 `D3D9DeviceEx*`，
需要真正减少依赖，而不是复制转换器形成第二套实现。旧 shipping 路径先保持行为与默认值不变。

### D1：有限但真实的 D3D9 代理 + 宿主资源表

实现实际 x86 COM 工厂/device/resource 代理和 x64 decoder；复用当前 DXVK device/context/allocator。
首批支持固定的 windowed profile，显式协商格式、shader/FF 路径和命令能力。
Create、Get、状态设置、资源上传、draw、query/readback/Present 都有真实可观察结果。
开发 profile 未支持方法应明确拒绝；不能用默认 S_OK、空 draw 或硬编码成功 query 填满 vtable。

### D2：第一条游戏无关的端到端 draw

独立 x86 程序实际加载候选代理 DLL，执行：

```text
Direct3DCreate9 → CreateDevice → GetCaps
  → CreateVertexBuffer/CreateIndexBuffer → Lock/写入/Unlock
  → CreateVertexDeclaration + 原始 D3D9 VS/PS token（或明确共用的 FF 路径）
  → SetRenderTarget/Viewport/RenderState/StreamSource/Indices/ShaderConstants
  → BeginScene → Clear → DrawIndexedPrimitive → EndScene
  → Issue event query → 按真实完成 GetData → GetRenderTargetData/Lock 比对
  → Present → Release
```

同一测试输入走既有单进程 x86 DXVK 作为对照。使用有几何覆盖、深度/混合与纹理变化的非平凡图案，
并有独立预期图/关键像素，不把重复 hash 或清屏成功当 draw 正确。
宿主必须调用当前 DXSO/FF 与 DxvkContext/DXVK Presenter，而非另写一个特制 Vulkan triangle backend。
测试程序的 x86 路径不得创建本地 Vulkan device；这才证明此受限绘制负载实际在 x64。

### D3：在接 Warcraft 前闭合必要反例

- 超过 64 KiB 的上传、分片截断/重复、错误 resource type/generation、shader 无效长度/阶段。
- UP 返回后立即覆写原数组；DISCARD 后旧 draw 仍正确；NOOVERWRITE、部分纹理 pitch、dirty 更新。
- 本地 Get/stateblock/COM identity、Release 后仍在途、对象反复创建销毁、query S_FALSE→完成。
- 默认资源未释放导致 Reset 拒绝；合法 Reset/resize 恢复；ResetEx 与普通 Reset 分别检查。
- 慢消费者下合法 draw 不丢、内存有界、背压可测；对端退出/协议错误/超时不悬挂、不伪成功。
- 原窗口消息泵、DXVK Presenter、退出/创建失败半初始化恢复；真实资源和线程收口检查。

### D4：按完整性矩阵接 Warcraft 与 WarVK pass

完成上述链路后再接 x86 游戏采集及固定功能/完整材质覆盖，迁移现有 WarVK 阴影、光照、雾、天气、
后处理与截图消费者。每次迁移明确输入、所有者、资源寿命、等价图和回退，避免双后端同时绘制。
原生语义不是省掉 D3D9 兼容工作的捷径：
[native input checkpoint](../research/2026-09-30-native-render-input-collection-checkpoint.md) 第 18-20 行
指出 shaderMask 只是 presence、只有 stage/sampler 0，并固定列出实例寿命、MDX geoset、GEOA、
动画时钟、纹理像素及全 pass 六项缺失，`sharedRendererReady=false`。
Stage11 阴影样本不能代表 forward、粒子/ribbon、UI、战争迷雾等完整游戏画面。

现阶段可以并行补 native 输入，但不能据此跳过全 D3D9 迁移或改称 FORGE 共用 GPU 引擎已经就绪。
共享核维持明确上游与单一实现，本文件不授权修改/发布其私有仓库。

## 7. 验收、账户与对外表述

每个阶段都记录实际 source/EXE/DLL 身份、adapter、配置、输入与结果；不继承别的候选测试。

| 门 | 最少证明 | 不能代替 |
| --- | --- | --- |
| 源码/编译 | x86 Hook 与 x64 通用代码分界；两位宽真实构建；同源执行逻辑 | D3D9 draw 已执行 |
| CPU 协议 | 真实调用接纳/拒绝、序号、所有权、背压、COM/Reset 正反例 | GPU 完成或游戏覆盖 |
| D3D9→GPU | 独立 x86 D3D9 输入、真实 DXVK 消费、独立像素及现有路径对照、资源退休 | Warcraft 正式场景/视觉验收 |
| WSI | 原 HWND 上现有 Presenter 的正常、resize、关闭、错误与无死锁 | 后台截图等于玩家前台体验 |
| 游戏覆盖 | terrain/unit/static/skin/附件/粒子/UI/雾/天气等实测；合法对象持续绘制 | 全场景“没有报错” |
| 生命周期 | 冷启动、同进程 A→B→A、压力解除恢复、异常退出、旧代际拒绝 | 清掉所有资源/全关阴影等于修复 |
| 成本 | 同输入的 x86 VA/private/commit 降幅、x64 与合计成本、p95/p99、拷贝/分配/锁/等待 | x64 能分配大块即已有玩家收益 |

至少导出：接纳/执行/完成命令数（分别定义）、待传/宿主已拥有/在途 GPU 字节、活跃/退役资源、
上传拷贝字节、x86 backing 与映射高水位、backpressure 次数/时长、typed fault、device loss 与恢复状态。
正常门的合法 draw 丢失必须为零；错误门必须证明确实拒绝且有可观察的错误结果。

本文件完成的是可实施边界，不是上述任一运行门。具体最小下一步为 **D0+D1，随后 D2 的真实
D3D9 draw**；RH1/RH2/RH3 作为依赖与回归保留，不再把新增独立 lab demo 当作渲染搬迁进度。
