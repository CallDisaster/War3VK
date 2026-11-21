# x64 代理线 P2：Codex 的独立只读审查（原文）

日期：2026-11-18。审查者：Codex（GPT 6.0 Astra，只读沙箱，新会话；作者指定的线程当时被桌面应用占用，没有写入）。
提示词要求：只读、不构建、不跑游戏或门禁、不提交、不碰玩家目录；重点找“宿主会拒绝而 x86 没有先挡住”的异步记录。
对每条发现的核对与处置见 `2026-11-18-x64-p2-async-design.md` 第 9 节。下面是报告原文，未改动；其中的行号是审查当时的工作树。

---

**WarVK x64 代理线 P2 独立只读审查报告**

审查对象是 `E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk` 当前未提交工作树，行号以本轮读取的源码为准。

**结论：不建议现在把四个 P2 开关一起切为默认开启。** 找到一条明确符合本轮核心规则的缺陷：窗口缩小后应用旧 StateBlock，前端能够异步发送宿主必定拒绝的 `ClearRegions`，把一次可局部处理的参数问题升级为整个会话终止。此外，`PRESENT_AHEAD` 扩大了最后一帧与窗口销毁之间的寿命缺口；场景软拒绝后的状态清理、旧新两端的能力识别也需要补齐。

另一方面，**没有找到正常纹理上传路径中压缩块、mip、部分矩形、上传事务或句柄代际检查不一致而必然触发异步拒绝的另一条路径**；没有发现两种环等待切换导致永久丢失唤醒，也没有发现资源表扩容后继续使用旧槽位地址的路径。

本轮仅做源码、文档及相关测试源码的静态阅读和搜索，没有修改或新建文件，没有运行构建、测试、游戏、宿主或门禁，没有提交、推送、部署或结束进程，没有访问玩家目录，也没有读取 `AutoTest/artifacts`。冻结画面门与三图实跑通过属于用户提供的背景，本轮没有独立复核那些运行产物。以下“确定”表示所列条件成立后，可以由代码确定结果，不表示已经在游戏中复现。

**一、按严重程度排序的发现**

**1. P1：旧 StateBlock 恢复过大的 viewport 后，`ClearRegions` 会被异步发送并导致会话终止**

位置：

- [d3d9_proxy_stateblock.h:74](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/tools/render_host/d3d9_proxy_stateblock.h:74)：`Device::apply` 的预检查没有检查 viewport；第 106 行直接赋值。
- [d3d9_proxy.cpp:1526](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/tools/render_host/d3d9_proxy.cpp:1526)：自动窗口缩放通过 `ResetKeepResources` 保留资源，第 1546 行把当前 viewport 更新为新尺寸。
- [d3d9_proxy.cpp:1906](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/tools/render_host/d3d9_proxy.cpp:1906)：Clear 的矩形处理仅使用当前 viewport 和可选 scissor，没有再与实际目标尺寸求交。
- [d3d9_clear_wire.h:30](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/tools/render_host/d3d9_clear_wire.h:30)：宿主解码时以实际目标尺寸检查每个矩形。
- [d3d9_host_main.cpp:1284](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/tools/render_host/dxvk_core/d3d9_host_main.cpp:1284)：异步拒绝发布 `AsyncRefused`。

**触发序列：**

1. 目标尺寸为 `640×480`，记录一个只包含 `SetViewport({0,0,640,480,...})` 的 StateBlock。
2. 窗口缩小为 `320×240`。`StateBlock::Apply()` 先调用 `followWindow()`，自动缩放成功；这条路径使用 `ResetKeepResources`，不会因为 StateBlock 仍存活而拒绝。
3. `Device::apply()` 第 106 行把旧的 `640×480` viewport 直接恢复，绕过 `SetViewport()` 第 326 行的目标范围校验。
4. 在 scissor 关闭时调用 `Clear(0, nullptr, D3DCLEAR_TARGET, color, 1, 0)`。
5. 前端生成 `640×480` 的清屏矩形。由于它不等于当前 `320×240` 全目标，走 `ClearRegions`，并在 `ASYNC_FLUSH` 开启时异步发送。
6. 宿主 `DecodeClearRegions()` 返回失败，`Session::apply()` 返回 `D3DERR_INVALIDCALL`，环循环发布 `AsyncRefused` 并停止。

**后果：** 前端没有先挡住宿主必拒绝的异步记录。这是本轮最直接的默认开启阻塞项。同样的旧 viewport 后续也可能影响绘制准入，但仅上述 Clear 序列就足以触发终止。

**建议改法：** 在 `Device::apply()` 修改任何状态之前，按照当前目标尺寸预检查将恢复的 viewport；同时在 Clear 的最终发送边界对矩形执行目标范围裁剪或共用 `ValidClearRegion` 校验。仅依赖 `SetViewport()` 已经校验过不成立，因为目标尺寸可以在捕获与 Apply 之间变化。若选择拒绝 StateBlock，应在所有状态修改之前拒绝，避免部分应用。

**把握：确定。** 不依赖设备丢失、损坏句柄或恶意环数据。

---

**2. P1：`PRESENT_AHEAD` 返回后立即销毁窗口，未执行的最后一个 Present 会把正常退出时序变成宿主终止故障**

位置：

- [d3d9_connection.h:1048](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/tools/render_host/d3d9_connection.h:1048)：发布当前 Present 后只等待上一帧；首个 Present 的 `previous` 为零。
- [d3d9_proxy.cpp:202](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/tools/render_host/d3d9_proxy.cpp:202)：窗口过程没有针对待执行 Present 的关闭前排空协议。
- [d3d9_backend.cpp:573](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/tools/render_host/dxvk_core/d3d9_backend.cpp:573)：`validateWindow` 要求 HWND、PID/TID 和会话标记仍有效。
- [d3d9_backend.cpp:2655](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/tools/render_host/dxvk_core/d3d9_backend.cpp:2655)：窗口验证失败返回 `D3DERR_DEVICELOST`；有资源或在途工作时可能进入 quarantine。
- [d3d9_proxy.cpp:1406](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/tools/render_host/d3d9_proxy.cpp:1406)：`Finish` 在设备析构末尾发送。

**触发时序：** x86 发布 `Present N`，上一帧已经完成，因此返回 `D3D_OK`。宿主尚未执行 `Present N` 时，窗口所有者线程执行销毁窗口；随后应用才释放 D3D 资源和设备。宿主到达 `Present N` 后，`IsWindow` 或窗口标记检查失败。

**后果：**

- 最后一帧不会呈现。
- 宿主报告 `DeviceLost`，停止处理后面的 Destroy/Finish。
- `canCleanWindowFault()` 要求没有活资源、没有 pending/commands 等条件；正常退出前尚有纹理、缓冲或帧内工作时不满足，可能走 quarantine 和宿主自身的 fail-stop。
- “后续 Finish 是同步屏障”无法修复这个顺序：屏障到达前，前面的 Present 已经失败。
- 若游戏已经不再调用 Present，前端依赖 Present 回调交付的终止通知也未必再有机会展示。

这不表示“最后一个 Present 一定丢失”。**先完成同步资源释放/Finish、再销毁窗口的顺序是安全的**；缺口是当前代码没有把这种顺序变成可保证的窗口寿命合同。

**建议改法：** 在 HWND/会话标记失效之前，由有权限的所有者执行明确的呈现排空；或者设计经过认证的正常关闭协议，明确处理尚未执行的呈现尾部。不能简单把任意 HWND 身份丢失都视为 `PresentSkipped`，否则会削弱当前身份校验。

**把握：确定所列时序会进入终止路径；当前游戏退出流程命中频率未验证。** 这是提前返回新增的实际寿命窗口，建议作为 `PRESENT_AHEAD` 默认开启前的必处理项。

---

**3. P2：新的异步结果语义和资源上限没有能力协商，旧新两端可以握手成功后才失败**

位置：

- [d3d9_command_ring.h:21](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/tools/render_host/d3d9_command_ring.h:21)：环版本仍为 `1`。
- [d3d9_command_ring.h:155](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/tools/render_host/d3d9_command_ring.h:155)：`ValidConsumerBlock` 不验证新语义能力。
- [d3d9_connection.h:877](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/tools/render_host/d3d9_connection.h:877)：Attach 请求只携带容量、回复容量和环版本。
- [d3d9_host_main.cpp:1238](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/tools/render_host/dxvk_core/d3d9_host_main.cpp:1238)：没有协商场景软拒绝或异步 PresentSkipped 能力。
- [d3d9_wire.h:155](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/tools/render_host/d3d9_wire.h:155)、[d3d9_resource_registry.h:17](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/tools/render_host/d3d9_resource_registry.h:17)：通用消息仍使用 D9C2/version 2，但最大资源 ID 已改为 65536。

**触发条件：** 新前端启用 P2，却加载了遵循 P2 前“异步非成功即终止”合同的旧宿主；或者新前端与资源上限仍为 16384 的旧宿主配对。

**后果：**

- 初始布局、nonce、容量和版本检查可以全部通过。
- 旧宿主遇到异步场景拒绝，或者异步 `PresentSkipped`，可能仍按终止故障处理。
- 新前端读取旧宿主的原保留字节，只能一直看到零，不能据此确认宿主支持 `sceneRefused` 语义。
- 当资源 ID 超过旧端上限时，旧端可在消息头解码阶段拒绝；当前握手不会提前暴露这一不兼容。
- 这种问题是语义不兼容，不能靠“ConsumerBlock 大小没变”证明兼容。

反方向的旧前端配新宿主，在仍使用同步场景消息、较小 ID 域的路径上可以保持旧行为。不能因此推导两个方向均安全。

**建议改法：** 给环语义升级版本，或在初始化/Attach 时协商所需能力与资源上限；在启用 `ASYNC_SCENE`、`PRESENT_AHEAD` 前确认对端支持相应例外。若产品要求严格同版本配对，应在连接建立时验证，而不是依赖部署习惯。

**把握：确定缺少协商；具体旧二进制的行为取决于其实现。** 本轮没有读取或运行旧二进制，以上旧端分支按其对应合同说明。

---

**4. P2：场景软拒绝发生在 active world scene 内时，忽略后续 Exit 会让 SessionReset 永久被拒**

位置：

- [d3d9_host_main.cpp:836](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/tools/render_host/dxvk_core/d3d9_host_main.cpp:836)：`sceneStopped` 后忽略 StageMarker，包括 WorldSceneExit。
- [d3d9_host_main.cpp:1277](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/tools/render_host/dxvk_core/d3d9_host_main.cpp:1277)：首次拒绝只发布计数并设置 `sceneStopped`，没有清理 tracker/packet。
- [war3_effect_scene.h:282](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/src/d3d9/war3/effects/war3_effect_scene.h:282)：`SceneTracker::frameEnd()` 不清除 `active`，`reset()` 在 `active` 时拒绝。
- [d3d9_host_main.cpp:872](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/tools/render_host/dxvk_core/d3d9_host_main.cpp:872)：SessionReset 先经过上述 tracker 检查。
- [d3d9_connection.h:775](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/tools/render_host/d3d9_connection.h:775)：被拒绝的 SessionReset 请求会被丢弃。

**触发序列：** 在启用注解的会话中，先发送并执行 WorldSceneEnter；然后发送第二个 WorldSceneEnter。`Connection::stageMarker()` 只检查枚举合法性，没有 tracker 顺序检查，因此这条记录可以进入环。宿主拒绝重复 Enter，设置 `sceneStopped`。已经排在后面的合法 WorldSceneExit 被忽略。之后执行 Present，再请求 SessionReset。

**后果：**

- `sceneTracker.active` 持续为 true。
- Present 虽然清掉 scene/shadow packet，但 `frameEnd()` 不清 active。
- 后续 SessionReset 持续返回 `D3DERR_INVALIDCALL`；设备 Reset 也没有复位该 tracker。
- 地图 generation 和 content store 的清理请求无法完成。
- 基础 D3D9 呈现可以继续，因此现有“清屏、屏障、Finish 均成功”的软拒绝测试不能排除此问题。

**范围限定：** 当前产品数据层自己的 `FrameWorldScene()` 会防止重复 Enter，所以本轮没有证据认为健康的三图路径会产生上述重复调用。但它是当前前端公开发送入口允许的拒绝序列，且宿主明确承诺把场景拒绝作为非终止降级处理。这条降级路径尚未完全收口。

**建议改法：** 首次软拒绝时终止并清理增强场景的半帧状态：清 scene/shadow packet 与 caster account，并明确撤销 tracker 的 active/completed 状态，保留 generation 和永久 `sceneStopped`。或者允许 stopped 状态下的 SessionReset 执行纯清理。**这里需要的是清理，不是自动恢复发送。**

**把握：确定所列序列会卡住 SessionReset；正常产品数据层触发该序列的可能性未证实。**

---

**5. P2：资源表扩容 OOM 的返回值有处理，但宿主把它放到了 GPU 创建成功之后，导致可预检的 CPU 分配失败终止整个宿主**

位置：

- [d3d9_resource_registry.h:44](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/tools/render_host/d3d9_resource_registry.h:44)：`own()` 在 `bad_alloc` 时返回空。
- [d3d9_resource_registry.h:87](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/tools/render_host/d3d9_resource_registry.h:87)：`commitCreate()` 因此可以返回 false。
- [d3d9_host_main.cpp:566](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/tools/render_host/dxvk_core/d3d9_host_main.cpp:566)：提交失败被当作“不可能”的 invariant failure。
- [d3d9_host_main.cpp:669](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/tools/render_host/dxvk_core/d3d9_host_main.cpp:669)：纹理的实际 GPU 创建先于 registry commit。
- [d3d9_host_main.cpp:1527](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/tools/render_host/dxvk_core/d3d9_host_main.cpp:1527)：异常走宿主 fail-stop。

**触发条件：** 创建某个首次触及、需要扩大 registry vector 的 ID。`canCreate()` 读取“未触及、空闲、代际 0”而返回 true；GPU 资源创建成功；随后 `commitCreate()` 的 CPU vector 扩容失败。

**后果：** 整个宿主终止。这不是漏判空指针——调用方确实处理了 false——而是新的可失败分配被放到了原来被视作不可失败的提交阶段。

**建议改法：** 在 GPU 创建前准备好 registry 槽位容量/预留状态，使 GPU 成功后的 commit 不再分配。预检 OOM 应使用已定义的“无副作用拒绝”结果，或明确扩展前端 `ResourceReservation` 的可回滚结果集合；不能只改成返回一个当前前端并不认可的新 HRESULT。

**把握：确定，但依赖宿主内存分配失败。** 不属于正常上传的拒绝缺口，也没有发现该路径导致继续运行时使用悬空句柄；当前是保守终止。

---

**6. P3：帧时间直方图的单个字段线程安全，但整体不是一致快照；“两种模式每个 Present”也需要限定入口**

位置：

- [war3_render_state.cpp:28](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/src/d3d9/war3/state/war3_render_state.cpp:28)：先更新 bucket/over/max，最后增加 frames。
- [war3_render_state.cpp:41](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/src/d3d9/war3/state/war3_render_state.cpp:41)：先读 frames，再逐个读其他计数。
- [war3_control_plane.cpp:4593](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/src/d3d9/war3/tools/war3_control_plane.cpp:4593)：直接发布该组合。
- [d3d9_device.cpp:14379](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/src/d3d9/d3d9_device.cpp:14379)、[d3d9_device.cpp:34507](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/src/d3d9/d3d9_device.cpp:34507)：计数在普通 Present 包装中，直接 PresentEx 没有相同调用。

**触发时序 A：** 控制线程读到 `frames=N`，渲染线程随后写入第 N+1 个 bucket 并完成 frames 更新；控制线程再读取 buckets。

**后果 A：** 一份合法返回的 JSON 可以出现 `sum(buckets)+over != frames`，max/overUs 也可能属于不同采样时刻。原子操作避免了字段撕裂，并没有提供整份统计的一致性。提高单个 load/store 的内存序不能独自修好这个问题。

**触发序列 B：** 原进程内模式直接调用 `IDirect3DDevice9Ex::PresentEx`，或使用绕过 Device::Present 包装的 swapchain Present。

**后果 B：** 不能据此保证统计覆盖这些呈现入口。当前普通 Device::Present 路径每次调用一次，代理 observer 路径也每次调用一次；这个窄范围检查是通过的。

**建议改法：** 若这些统计用于严格 A/B 差分，增加一致快照协议，例如单写者版本号加读侧重试，并保留当前原子字段；同时把计数放到明确共用、且不会重复进入的呈现边界，或在输出 schema 中准确声明只覆盖当前普通 Device::Present 路径。

**把握：确定。** 属于诊断数据问题，不影响渲染输出。

---

**7. P3：`profileAhead()` 写入等待耗时却不增加对应样本数，导致 Present 的调用配对信息消失**

位置：

- [d3d9_connection.h:1468](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/tools/render_host/d3d9_connection.h:1468)：增加 `syncTicks`、histogram 和 pair ticks，但不增加 `syncCalls` 或 pair calls。
- [d3d9_connection.h:1531](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/tools/render_host/d3d9_connection.h:1531)：`p.calls==0` 的 pair 不输出。
- [analyze_x64_sync_profile.py:212](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/AutoTest/analyze_x64_sync_profile.py:212)：平均等待使用 `syncUs/sync`，零调用时输出空值。

**触发条件：** 一个稳定采样区间内全部 Present 都走 ahead，且没有同一 API/op 配对的同步 Present。

**后果：** Present 的等待时间仍出现在 op/API 总量中，但 API/op 的 pair 明细缺失，`sync` 为零而 histogram 有样本，原来的计数字段不再能作为等待样本数。这不是全部 Present 等待时间丢失，也不直接否定帧间隔 FPS 数据。

**建议改法：** 增加明确的 ahead 调用/等待样本计数及相应 pair 输出规则，区分“有同步回复的记录数”和“发生阻塞测量的样本数”。不要直接增加现有 `syncCalls` 来掩盖问题，因为那会破坏环同步记录数的恒等式。

**把握：确定。**

**二、异步纹理上传逐项拒绝条件对照**

以下检查基于实际发送入口 `Texture::send / UnlockRect / SetLOD`，没有把任意手工构造的环记录视为正常产品输入。

**所有相关记录共用的拒绝入口**

[d3d9_host_main.cpp:534](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/tools/render_host/dxvk_core/d3d9_host_main.cpp:534) 在进入各操作前还会拒绝：

- 前一条 DrawAnnotations 要求绑定的下一条消息不是当前消息。
- content blob 上传仍打开，当前消息却不是它的后续 ContentUpload。
- 注解引用的 content 不存在。
- 操作码、payload 长度/指针、资源 ID/代际组合非法。
- Session 已 finished，或尚未 initialized。

前端对应约束来自：

- [d3d9_connection.h:562](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/tools/render_host/d3d9_connection.h:562)：所有者、会话可用、初始化确认、允许异步的操作集合，以及发送前 `flushDraws()`。
- [d3d9_connection.h:653](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/tools/render_host/d3d9_connection.h:653)：ContentUpload 在同一所有者调用内连续发布所有 chunk。
- [d3d9_proxy.cpp:710](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/tools/render_host/d3d9_proxy.cpp:710)：资源命令从固定 ref 构造，其他 header 字段默认清零。

在正常前端入口中，没有找到把纹理记录插入上述注解绑定或 content 事务中间的路径。

| 操作与宿主拒绝条件 | x86 对照与结论 |
|---|---|
| **BeginTextureUpload：** 找不到相同 ID/代际的活 Texture；payload 非空；args[6…] 非零。宿主第 674–679 行。 | `resourceAllowed()` 在 proxy 第 713–716 行检查本地 live ref；创建成功回复后才设置 `remoteLive`；第 957–958 行只写允许的参数。销毁是同步的，成功回复后才复用 ID。查过，没有发现正常句柄路径的缺口。 |
| **Begin：** 已有一个上传事务；资源 revision 已达 UINT32_MAX；新 revision 不是旧值+1。宿主第 675–677 行。 | UnlockRect 第 951 行拒绝 revision 耗尽，第 957 行发送 revision+1，第 959 行准入后递增。一个 UnlockRect 在返回前发布完整 Begin/Chunk/Commit，所有调用限制在同一 owner，等待时不分发任意应用消息。不同纹理可同时持有本地锁，但 Unlock 的线事务串行。查过，没有发现正常事务重叠。 |
| **Begin：** mip、矩形、格式或块对齐不被 `MakeTextureRegion` 接受。 | LockRect 第 925、929–930 行检查级别、矩形，并调用 `MakeManagedLockRegion`；该函数最后调用宿主使用的同一个 `MakeTextureRegion`。不是两份独立近似检查。 |
| **Begin：** 某 mip 尚未初始化，却不是整级上传。宿主第 678 行。 | UnlockRect 第 956 行把第一次上传扩大为整级，使用该级本地零初始化 backing。之后部分上传才保留 patch 范围。查过，没有发现不一致。 |
| **TextureUpload::begin：** 已 active、ref/token/bytes 为零，或 bytes 超过 `1024×1024×4`。 | [d3d9_texture_wire.h:100](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/tools/render_host/d3d9_texture_wire.h:100)。正常创建布局和前述 revision/region 构造排除这些条件；最大合法区域不会超过该界。内部 vector 分配仍可能抛异常，见后文。 |
| **TextureUploadChunk：** 找不到活 Texture；args[2…] 非零；事务 ref/token 不匹配；offset 不等于已收字节数；data 为空；bytes 为零、超过 MaxPayload，或超过剩余空间。 | 宿主第 683–685 行及 texture wire 第 109–111 行。前端第 964–972 行从 offset=0 连续递增，`count=min(MaxPayload, remaining)`，每条使用同一资源 ref/revision 和实际数组 payload。查过，没有发现缺口。 |
| **CommitTextureUpload：** 找不到活 Texture；payload 非空；args[1…] 非零；没有匹配事务或字节尚未收齐。 | 宿主第 688–690 行。前端第 974 行仅在所有 chunk 准入成功后提交；异步中任一步失败都会终止，不会继续作为健康会话提交。FIFO 保证宿主先处理全部 chunk。 |
| **TextureLod：** 找不到活 Texture；payload 非空；args[1…] 非零；LOD 超过级数；该纹理正被上传事务占用。 | 宿主第 720–723 行。前端第 909–914 行检查资源、拒绝持锁状态并 clamp 到 `levels-1`；Unlock 发布 Commit 后才返回，随后 SetLOD 在环中位于 Commit 之后。查过，没有发现缺口。 |

针对用户指定的纹理分类，结论如下：

- **压缩格式与块对齐：查过，没有发现两端不一致。** MANAGED 的非块对齐锁范围会被扩大到触及的完整块；末端不足一个块的 mip 边缘由共用 `MakeTextureRegion` 接受。前端打包按 block-row 字节数推进。
- **非零 mip：查过，没有发现被误当成 level 0。** 级别进入 Begin，宿主及后端都使用该级 layout；后端 Vulkan subresource 的 mip level 来自 `checked.level`。
- **部分矩形：查过，没有发现首次与后续初始化判定不一致。** 首次上传整级；后续 patch 按该级 pitch 抽取紧密排列的行。
- **立方体和体纹理：目前不会进入这条异步路径。** [d3d9_unsupported.h:66](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/tools/render_host/d3d9_unsupported.h:66) 的创建入口返回 `E_NOTIMPL`。这是未支持的能力，不是异步上传已覆盖。
- **DEFAULT 池、动态纹理、渲染目标纹理：目前不会创建成功。** [d3d9_proxy.cpp:1616](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/tools/render_host/d3d9_proxy.cpp:1616) 要求 `usage==0` 且 `pool==D3DPOOL_MANAGED`。
- **DISCARD：在纹理 LockRect 本地拒绝。** 第 925 行只允许 READONLY 位。不能把缓冲区 DISCARD 的支持外推到纹理。
- **READONLY 锁：Unlock 不发送上传。** 第 950 行直接结束锁。
- **已销毁或代际不符：查过，没有发现正常 COM/registry 生命周期会发布陈旧 ref。** 销毁确认、代际保留及重用顺序一致。
- **Begin 与 Commit 之间被别的操作打断：当前正常前端没有找到该路径。** 宿主并非对所有无关操作都设置全局事务排他门，但前端 Unlock 的连续发布、单 owner 和不泵任意消息共同保证这里不插入 Draw/Reset/Destroy。这个结论依赖该线程合同，不能外推给未来并发发送入口。
- **Reset 前后：查过，没有发现 MANAGED 纹理的 revision、initializedLevels 或 ref 被单端清零。** Reset 是同一 FIFO 上的同步屏障；宿主拒绝仍打开的上传事务。纹理仍属于设备连接，不属于地图 generation。
- **设备丢失：无法由 x86 提前等价判断。** 发布前的 `usable()` 能拒绝已知终态；GPU 之后才发生的丢失、分配失败或提交失败必须仍走终止路径，不能列为“全部可在发送前挡住”。

**后端异常与 HRESULT 拒绝需要分开描述。**

[d3d9_backend.cpp:1762](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/tools/render_host/dxvk_core/d3d9_backend.cpp:1762) 的 `uploadTextureRegion()` 和第 1833 行的 `setTextureLod()` 返回 `void`。它们的额外失败包括：

- Owner 已结束或被 quarantine、错误所有者线程、typed slot 越界。
- 纹理 image 不存在、上传源为空、区域/字节数不一致、LOD 越界。
- 计数器耗尽。
- 上传事务、staging、image view 或命令列表等分配失败。
- 已知设备故障，timeline 查询/等待失败，CPU 提交、GPU 完成或资源退休失败/超时。
- 使用 host-owned surface 模式时，其额外窗口状态验证失败。

这些通常抛异常，外层终止宿主，**并不都先转换成 `Session::apply()` 返回非成功再发布 `AsyncRefused`**。因此“所有非成功都由 x86 提前校验”不是准确表述；可以成立的是：正常输入的显式准入规则目前基本对齐，运行时故障仍终止。

**三、异步 FlushDraws 与 ClearRegions**

`FlushDraws` 的显式宿主拒绝条件是非零资源 ID、非空 payload、非零 args，加上公共 header/session/注解/content 检查。前端 [d3d9_connection.h:710](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/tools/render_host/d3d9_connection.h:710) 先发送本地 staged draws，然后用零初始化 Header 构造 FlushDraws；只有 grouped/stream 配置需要时才发送该宿主记录。后端只调用 group/stream flush，失败主要来自提交和 GPU 状态异常。

**FlushDraws 的正常参数和发送顺序：查过，没有发现。** EndScene 将 scene 状态置为 false 后发送，与原同步路径的提交顺序一致；异步只改变等待返回的时机。

`ClearRegions` 的显式拒绝条件完整包括：

- 操作类型、资源 ID/generation 不符合要求；
- count 为零或大于 64；
- payload 为空，或长度不等于 `count×16`；
- flags 为零或包含目标/深度以外的位；
- args[4…] 非零；
- depth 非有限值或超出 `[0,1]`；
- 任一矩形为空或超出实际 target。

前端第 1898–1918 行已处理 no-op、flags、stencil、depth、count、应用矩形读取和空交集；header/count/payload 长度也由构造保证。**唯一找到的参数等价检查缺口是发现 1：实际目标范围不能始终由 viewport 的历史校验保证。**

后端 [d3d9_backend.cpp:1929](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/tools/render_host/dxvk_core/d3d9_backend.cpp:1929) 再校验相同域，随后 `begin()` 会 settle 前面的 stream/group 工作再执行 clear。没有发现它把清屏提前到之前绘制的前面。

**四、异步场景、停止状态及半帧状态**

**`sceneRefused` 的读取与发布竞争：查过，没有发现该竞争本身导致会话终止或恢复发送。**

允许的时序是：x86 读到零，宿主随后发布拒绝，x86 又发送一条场景记录。宿主单线程先设置 `sceneStopped`，然后才取下一条记录；格式合法的后续场景记录会在 packet/tracker 修改前被忽略。`sceneRefused` 使用 release 发布、acquire 读取，下一次观察到非零便本地拒绝。

前端停止恢复的实际规则也与设计一致：

- [war3_proxy_frame.cpp:129](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/src/d3d9/war3/platform/war3_proxy_frame.cpp:129) 的 `Sent()` 一旦失败便设置 `sendingStopped`。
- 第 528–541 行的 `FrameConnect()` 清除停止状态。
- 第 627–637 行的地图 SessionReset 不清除停止状态。
- 设备 Reset 的 observer 只处理 overlay reset，没有重新 FrameConnect。

因此，**地图切换或普通设备 Reset 不清 `sceneStopped/sceneRefused`，本身不是两端不一致；两端都采用“这个连接永久停止增强记录，新的设备连接才恢复”的合同。**

半帧状态方面：

- 正常 presenter 的 Present 在宿主第 988 行执行 `sceneTracker.frameEnd()` 和 `clearScene()`。
- 已接受的 SessionReset 也清 scene/shadow packet。
- stopped 后没有找到继续拿残留 packet 执行 BeforeUi 的路径。
- 设备 Reset 本身不清这些 Session 字段；离屏 Present 在 `E_NOTIMPL` 返回前也不执行 presenter 后面的清理。停止状态下这通常只是残留到下一次有效清理点，不会自动重放。
- **真正的缺陷是发现 4 的 active 状态不能收口，进而阻挡 SessionReset。**

现有 `scene-refused` 测试源码发送的是未协商 annotations 的 marker，拒绝发生在 tracker 修改之前；后续第二条也再次被入口拒绝。因此它没有覆盖“已进入 active scene、拒绝、忽略 Exit、之后 Reset”的状态。

**五、Present ahead、同步屏障与等待协议**

**领先帧数应分两种口径。**

从 [d3d9_connection.h:1051](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/tools/render_host/d3d9_connection.h:1051) 可以直接得出：

- **成功返回时：** 上一个 ahead Present 必须已经 applied，因此最多剩当前一个 Present 未完成。查过，没有发现成功返回后继续无界领先的路径。
- **等待过程中：** 代码先发布 `Present N`，再等待 `Present N−1`，所以环中可以同时有两个未完成的 Present。这是正常执行路径。
- 若“最多领先一帧”的验收定义是“待执行 Present 数永远不超过 1”，当前代码不满足；若定义是“应用从 Present 返回后最多留一帧未完成”，当前实现满足。报告与门禁应明确口径。

**`presentAheadSequence`：查过，没有发现当前生命周期内需要在 Reset 时清零。** Reset 不重置环序号；同步 Reset 成功时，旧 ahead 序号已经包含在 applied 前缀里。当前连接只在 Initialize 后附着一次环，宿主也只允许那个位置 Attach；没有成功的同会话重挂接/序号归零路径。会话失败后不恢复使用，新 Connection 的成员从零开始。未来若新增重连，届时必须重新定义这些值，不能直接复用当前假设。

**同步回复错配：查过，没有发现。** Readback、GetRenderTargetData、Reset、Destroy、Finish 在同一 FIFO 中发布同步记录，等待自己的 sequence。宿主仅为同步记录写回复槽；前端在同步返回前不再发布下一条同步记录，回复身份还要通过 `DecodeReply` 检查。排在前面的异步 Present 不会覆盖该同步回复。

**离屏入口：查过，没有发现另外绕过的产品入口。** Factory 创建 Connection 的两处都使用 `PresenterProfile()!=Offscreen`；构造函数第 128 行执行 `presentAhead=presentAhead&&presenter`。需要 presented readback 的调用携带结果缓冲，`call()` 第 795 行不会分流到 ahead。

**挂起与退出检测：** 等待上一帧时会重新检查 applied、宿主 Faulted 状态和本地健康状态；没有进展由滚动 10 秒时限处理，回复阶段还有调用期限。worker 同时监视宿主进程句柄，宿主退出会通知生产者。因此没有找到“宿主永久挂起后仍永久睡眠”的正常控制流。

但不能把它表述为“整个 Present 严格最多 30 秒”：`ringPublish()` 的空间准入使用滚动无进展限制，不传入这次调用的绝对截止时间；宿主持续缓慢推进时，准入可以超过 30 秒。这是当前等待合同的限制，不是本轮新增的丢失唤醒。

**waiting=1/2 切换和提前唤醒：查过，没有发现永久丢失唤醒。**

关键顺序是：

1. 生产者设置 waiting，随后重新观察 head/applied。
2. 宿主复制完整记录到私有 packet 后，以 seq_cst 发布 head，再检查是否等待空间。
3. 执行完后发布 applied，再检查任意非零 waiting。

在默认 wake fix 开启时，第一次 `ringAwait()` 只建立基线并返回，调用者重新检查条件。之后发生 2→0→1 切换，或消费端在切换间隙发出通知，都不会把事件本身当成条件成立：

- 等空间醒来后重新 `tryWrite()`；
- 等完成醒来后重新比较 applied；
- 提前 head 通知不会被误当作 GPU/Present 已执行；
- 遗留的自动复位事件最多造成一次额外醒来。

显式关闭 `WARVK_D3D9_RING_WAKE_FIX` 仍会恢复已知的首次基线窗口，可能多睡一个 Recheck 间隔；本轮没有找到新 space 标记与默认修复互相冲突的路径。

**六、ConsumerBlock 的 sceneRefused 布局与初始化**

**当前声明的布局：查过，没有发现明显错位或非对齐成员。** `sceneRefused` 放在 140，后面的 64 位 `faultSequence` 仍在 144，总大小仍为 192。共享映射按控制页起点放置结构体。

初始化路径也清楚：

- [d3d9_ring_channel.h:73](E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/tools/render_host/d3d9_ring_channel.h:73) 创建全新的命名映射，拒绝已存在对象。
- 宿主第 109–111 行要求 consumer 区域初始全零，再 placement-new `ConsumerBlock{}`。
- 因此正常新会话的 sceneRefused 为零；失败后不会复用旧映射恢复同一连接。
- `InitializeConsumerBlock()` 单独只写常量，不负责重置所有原子字段。当前生产调用安全，是因为它之前明确完成了值初始化；不能把该 helper 单独理解成“重置整个控制块”。

仍建议补充：

- `offsetof(ConsumerBlock, sceneRefused)==140`；
- `offsetof(ConsumerBlock, faultSequence)==144`；
- 原子成员大小、对齐和结构体布局断言；
- 对新字段的布局测试与初始零值检查。

当前生产 static_assert 主要检查结构总大小；现有布局测试源码检查了相邻字段，却没有把 sceneRefused 单独钉住。**这属于证据与防回归缺口，不是本轮已经发现的 ABI 错位。** 旧新混用的主要问题是发现 3 的语义能力，不是立刻发生内存重叠。

**七、资源表放大与 vector 生命周期**

**槽位地址失效：查过，没有发现。**

`slots` 私有；返回槽位引用的 `at()` 也仅内部使用。`reserve()` 在调用可能 resize 的 `own()` 前已完成只读判断，没有保留旧槽位引用；`commitCreate()` 同样如此。`matches/next/operator==` 不进行增长，`cancelReservation/commitReservation/destroy` 操作的槽位已实际存在。

**`own()` 的空返回：两个直接调用者都检查了。** 前端 `reserve()` 失败返回空 ref，资源创建入口返回 OOM；宿主 `commitCreate()` 失败由 Require 终止。后者的提交时机问题见发现 5，没有发现空指针直接解引用。

**未触及槽位与原固定数组语义：查过，没有发现差异。**

- 未触及槽位读为 Free、None、generation 0。
- 首次只能创建 generation 1。
- Reserved 槽位必然已经实际分配；不能把未触及槽位误认成 reservation。
- 取消预留不消耗 generation。
- Destroy 保留 generation tombstone。
- 达到 MaxGeneration 的槽位不再重用，没有代际回绕到零。
- `lowestFree` 的更新不会跳过一个仍可分配的更低 ID；`commitCreate()` 不推进该提示只会影响扫描效率，不改变正确性。

**两端上限与掩码：查过，没有发现当前同版协议仍把资源 ID 截成 14 位或 16 位。**

- 共用 `MaxResources=65536`。
- wire ID/generation 均为 32 位。
- 宿主 entries 使用 `MaxResources+1`，包括末端 ID。
- 后端纹理 typed slots 为 `MaxResources-64`，与逻辑 ID 不是同一命名空间；槽位不足在 Create 阶段返回拒绝。
- `entryLimit` 为最高写入 ID+1；`textureLimit` 对应一基 typed ID，扫描时使用零基数组 `<textureLimit`，没有找到 off-by-one。
- 搜到的 `MaxContentBlobs=16384` 属于独立 content blob 表，不是资源 ID 上限。
- 没有找到资源协议里残留的 `0x4000` 上限或 14 位资源掩码。

有一个表述应收紧：只有 registry slots 改为按需增长；宿主 entries 和后端 textures 仍有按最大值分配的固定数组。因此“上限本身不花成本”不能作为整个宿主的结论。高水位扫描也不会随释放自动缩小。本轮未测内存与扫描耗时。

**八、ExitText、Palette Watcher 与流分段提交**

**ExitText 的本轮修订：查过，没有发现正常路径下的短写循环或 va_list 使用错误。**

- 计算格式化长度使用 `va_copy`，分配失败后的直接输出没有复用已消费的原参数列表。
- 正数短写继续写，零写停止并标记 incomplete。
- 完整写入后检查 `fflush`。
- 正常出口在 session.reset 前显式 flush，并检查 complete。
- 32 KiB 是到行尾才触发的阈值，不是精确的最大驻留字节数；还需加上当前尚未结束的一行。
- 析构 flush 覆盖正常异常展开，不能保证硬终止/访问异常后的尾部日志完整。

短写、零写和 OOM 降级分支本轮没有运行验证。

**ClosePaletteWatcher 的收紧：查过，没有发现明确的跨进程误投递路径。**

当前要求本进程内恰好一个匹配窗口，打开对应线程句柄，并在 PostMessage 前再次核对 PID/TID/类名；未找到匹配或复核失败就不发，等待为 500 ms。线程句柄在等待期间保持有效。

它仍不是原子窗口身份操作：复核与 PostMessage 之间存在理论竞争，而且类名不等于模块身份。但本轮没有证据把这种残余风险升级为已发现的可触发缺陷。该机制仍依赖 opengl32 的非公开行为；500 ms 也会占用父进程最终退出预算，应保留独立测量。

**STREAM_FLUSH_DRAWS 的同帧顺序和状态：查过，没有发现“后续阶段要求所有本帧绘制仍在当前列表”而被新分段破坏的路径。**

依据如下：

- 两处绘制路径在新 batch 开始前 flush，batch 内仍共享一个列表。
- 新 batch 的第一个 draw 不使用前一 batch 的差分状态指针；后续列表能重新绑定状态。
- `flushStream()` 提交同一队列，保留完成 timeline 和资源引用，重置的是当前列表计数。
- 后续纹理上传仍在流中顺序记录；普通 clear 经 `begin()->settle()` 等待此前工作。
- scene/shadow packet、caster scene/account 不随 flushStream 清除，BeforeUi 不从“当前列表有哪些 draw”反推整帧 caster。
- `runBeforeUi()` 可以重新打开 stream，并通过 external rendering 接到当前上下文。
- `DxvkContext::endFrame()` 在这里仅重置 render-pass index；真正的游戏场景清理仍由 Present/FrameEnd/SessionReset 执行。

需要准确描述其阈值：它不是“刚记录到 N 个 draw 就立即提交”。当前在**下一个 batch 开始前**检查旧计数是否达到 N，故可能越过 N 至多一个 batch；若后面没有新 batch，则仍等 EndScene/Present。这影响收益与阈值解释，不构成绘制次序错误。

**九、帧时间统计的其余边界**

除发现 6、7 外：

- **正常单所有者路径的字段读写：查过，没有发现非原子计数被控制线程直接读取。** `m_paceFrequency/m_paceLast` 由 beginFrame 写者使用，控制线程只读原子统计。
- **单写者前提仍是前提。** `RenderState` 是全局 singleton，普通字段没有多写者保护；若进程内模式由不同设备/线程并发进入 beginFrame，会产生数据竞争。当前代理限制 owner，本轮没有证据证明 Warcraft 产品路径会产生多写者。
- **普通路径计数：** 进程内 `Device::Present` 调一次 beginFrame，再进入 PresentEx；代理在 observer 的 ObservePresent 中调一次，未看到同一路径重复计数。直接 Ex/swapchain 入口的缺口见发现 6。
- **统计含义：** 它记录相邻 beginFrame 的 CPU 间隔，首帧没有间隔样本；包含菜单、加载和暂停后的长间隔，不等于只统计 world frame，也不等于真实显示完成间隔。
- **溢出：** bucket 是 32 位累积值，单 bucket 到 `2^32` 次会回绕；frames 是 64 位，不会同步回绕。高帧率超长运行需要处理。`deltaTicks×1,000,000` 也没有乘法溢出保护；例如 QPC 为 10 MHz 时，单次间隔约超过 21 天才会触及该问题，普通帧间隔不接近此界。
- **热路径代价：** 常规每帧增加一次 QPC、一次 64 位换算除法、bucket 和 frames 的原子累加，以及 max 检查；没有每帧堆分配、互斥锁或 400 项遍历。400 bucket 的读取和 JSON 构造在控制请求路径。只能给出静态成本结构，本轮不能给出实测“零开销”结论。

**建议在切默认之前必须处理**

1. **修复发现 1。** 把 StateBlock 恢复后的 viewport 与最终 ClearRegions 发送域闭合；后续验证必须包含“窗口缩小、旧 StateBlock Apply、Clear”，并同时检查合法清屏继续工作。
2. **为 PRESENT_AHEAD 建立最后一帧与 HWND 销毁的寿命合同。** 覆盖最后 Present 尚未 applied 时关闭窗口、有活资源时退出，以及在同步 Finish 前后关闭窗口的不同顺序。
3. **明确旧新端配对。** 通过协议版本或能力协商拒绝不支持场景软拒绝、异步 PresentSkipped、资源上限的组合；不能只验证布局大小。
4. **补齐场景软拒绝的半帧清理。** 至少覆盖 active scene 内拒绝、随后 Exit 被忽略、Present、SessionReset、设备 Reset；保持发送永久停止的原合同，同时保证地图清理不被卡住。
5. **统一“一帧领先”的验收定义。** 当前返回时最多一个未完成 Present，等待期间可以有两个；若要求任何时刻只允许一个，必须调整发布/等待顺序。
6. **补做针对新增分支的验证，而非仅重复正常画面哈希。** 重点是 PresentSkipped、拒绝后的 ignored 分支、空间等待与 applied 等待切换、退出尾部和上述旧 StateBlock 序列。本轮没有执行这些验证。

按开关分别看，`ASYNC_UPLOADS` 未发现正常上传参数的确定性拒绝缺口；`ASYNC_FLUSH` 被发现 1 阻塞；`ASYNC_SCENE` 应先处理状态清理与能力识别；`PRESENT_AHEAD` 应先闭合窗口退出寿命与计数口径。

**可以以后再做**

- 将宿主 registry 扩容前移到无 GPU 副作用的预检阶段，避免发现 5 的晚期 OOM 终止。
- 为 framePace 提供一致快照、覆盖范围声明、长期累积溢出策略，并明确单写者约束。
- 给 ahead Present 增加独立等待样本数和 pair 统计，保留同步记录数的原语义。
- 为 ConsumerBlock 新字段补齐 offsetof、对齐和初始值防回归检查。
- 把 STREAM_FLUSH_DRAWS 文档改为 batch 边界阈值，并另测收益、提交次数、尾延迟和分配成本。
- 补充 ExitText 的短写/OOM 故障注入和 Palette Watcher 的退出预算观测；没有必要把本轮未证实的理论竞争描述成已发生问题。
- 修正“资源上限本身无成本”“所有宿主失败都能由 x86 提前检查”等过强表述，使诊断报告准确区分输入准入、CPU 分配、GPU 执行和进程终止。

