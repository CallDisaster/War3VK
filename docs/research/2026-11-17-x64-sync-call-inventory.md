# x64 代理模式：同步往返清单与 P2 方案（2026-11-17）

为“P2：没有结果要拿的调用改成异步，Present 领先不超过 1 帧”做准备。只读调研，没有构建、没有跑游戏或宿主。

标记：**已核对** = 读过代码或直接读到计数器；**推断** = 由计数器相减、或由代码推出来，没有直接测过。

Remix 代码引用钉在 dxvk-remix `afb0252`（2026-11-16 的 main），下文写成 `R/<路径>:<行>`，
完整链接见文末“引用”。

---

## 0. 先说结论

1. **同步往返的九成是纹理上传。** 前端每次 `UnlockRect` 发 `BeginTextureUpload`、每 64 KiB 一个 `TextureUploadChunk`、
   `CommitTextureUpload`，三条全都等回答（`tools/render_host/d3d9_proxy.cpp:914-944`，已核对）。
   Lost Temple 和 EchoIsles 每个 Present 有约 27–28 次纹理提交，即约 81–84 次同步，占全部同步的 88–89%（推断，算法见 §2）。
   数据层的帧记录与阶段标记只占 7%，Present 只占 1%。
2. **宿主那边，每次纹理提交还要等 GPU 空两次。** `uploadTextureRegion` 先 `settle`（把流提交出去，等 GPU 做完、资源退役），
   再 `begin`，录一次拷贝，`submit(waitForGpu=true)` 再等一次（`dxvk_core/d3d9_backend.cpp:1408-1432`、`438-479`、`685-709`，已核对）。
   每个 Present 56–59 次 GPU 提交里，约 54 次是它带来的（推断）。
   所以只把 x86 这头改成不等，宿主仍然一帧排空 GPU 五十多次。**P2 要和“宿主上传不排空”一起做，收益才完整。**
3. **错误晚报的机制已经有了。** 异步记录被拒，宿主发布 `Fault::AsyncRefused` 后退出；x86 下一次调用在 `usable()` 里看到，
   走 `failClosed(HostRejected)`，下一次 Present 把终止报告交给数据层，游戏退出（已核对，§3）。
   P2 只需补三件事：宿主把“软结果”当成功、x86 预判今天可以回滚的失败、终止报告里写出被拒的是哪条操作。
4. **任务书里有几处数字和计数器对不上**（§1.2）：这轮门用的环是 **4 MiB**，不是 16 MiB；
   “帧”按宿主 Present 数是 2,118 / 2,249 / 9,647，不是 1,600 / 1,770 / 1,240；
   生与死每个游戏内帧的环字节是 6–12 MB，不是 6.4 MB。
5. **Remix 的做法与我们要做的基本一致**（只有要数据的调用才等，Present 领先 1 帧，上传完全异步），
   但它的失败模型是“记日志、继续跑”，我们的是“失败即终止”，更严。3f75d64 撤回了 HL2 的异步创建，原因没写，
   我们做异步创建时要谨慎（§6）。

---

## 1. 口径

### 1.1 数据从哪来（已核对）

5.2 A/B 门第 1 轮，`AutoTest/artifacts/x64-phase52-ab-20261117-r1/<图>-r1-proxy/`：
`game.stderr`（前端 JSON 行）、`host.log.2`（宿主 JSON 行）、`receipt.json`（控制面帧序列、子进程环境变量）。

这轮门开着的开关（`receipt.json` 的 `childEnvironment`，已核对）：
`WARVK_D3D9_COMMAND_RING=1`、`BATCH_DRAWS=1`、`ASYNC_BATCH=1`、`FIXED_BATCH=1`、`ASYNC_EVENTS=1`、`STREAM=1`、
`ANNOTATIONS=1`、`X86_BACKING_KIB=32768`、`UPLOAD_WINDOW_KIB=1024`、`PROFILE=presenter-isolated-v1`。
没有设 `COMMAND_RING_KIB`，所以环是默认的 4 MiB（`d3d9_command_ring.h:25`；`command-ring-proxy` 行 `"capacity":4194304`）。

### 1.2 帧数：按 Present 算，与按世界帧算

| | Lost Temple | EchoIsles | 生与死 |
|---|---|---|---|
| 宿主 Present（`presentAttempts`） | 2,118 | 2,249 | 9,647 |
| 世界帧（`worldSceneEnters`） | 1,655 | 1,812 | 636 |
| 进图后帧率（控制面帧序列，首末世界样本） | 27.4 | 29.2 | 9.1 |
| 同步调用（`syncCalls`） | 195,650 | 213,602 | 181,753 |
| 同步 / Present | 92 | 95 | 19 |
| 同步 / 世界帧（上限，含载入期） | 118 | 118 | 286 |

生与死在进图前有一段约 7,400 帧、约 290 fps 的阶段（控制面帧序列 48–73 秒，世界标志为假），
所以按 Present 平均会把它的游戏内负担摊薄很多。下文“每帧”都说明是按 Present 还是按世界帧。

---

## 2. 同步从哪来：按计数器拆

算法（推断，所用口径已核对）：
- 纹理提交次数 = 宿主 `uploads` − 动态缓冲的 `noOverwriteUploads` − `discardUploads`。三张图都只有 3 个缓冲，全是动态的
  （`dynamicBuffers:3`、`orderedUploads:0`），而宿主 `uploads` 只在 `uploadBuffer` 与 `uploadTextureRegion` 里加一
  （`d3d9_backend.cpp:1367`、`1430`）。
- 每次纹理提交至少 3 次同步（Begin + 至少一块 + Commit）。
- 帧记录与阶段标记、Present、创建与销毁（`logicalHandlesCreated/Destroyed`）、诊断读回、`ReadContent`（`fetchCalls`）都直接有计数。
- 余下的是 EndScene 的 `FlushDraws`、大纹理的额外块、`ClearRegions`、`SetLOD`、`GetDeviceCaps` 等，没有单独的计数器。

| 同步来源 | Lost Temple | EchoIsles | 生与死 |
|---|---|---|---|
| 纹理提交次数 | 57,273（27.0 / Present） | 63,281（28.1 / Present） | 39,807（4.1 / Present，大半在载入期，推断） |
| → 同步，至少 3 倍 | ≥171,819（88%） | ≥189,843（89%） | ≥119,421（66%） |
| FrameScene + StageMarker | 13,240（6.8%；每世界帧 5 + 3） | 14,496（6.8%） | 5,088（2.8%） |
| Present | 2,118（1.1%） | 2,249（1.1%） | 9,647（5.3%） |
| 创建 + 销毁 | 4,048（2.1%） | 2,380（1.1%） | 31,592（17.4%；15,791 张纹理） |
| 读回（门的截图与帧转储，4 张 × 121 带） | 484 | 484 | 484 |
| `ReadContent`（逐出后取回） | 62 | 63 | 68 |
| 余数（EndScene、额外块、ClearRegions、SetLOD、Caps…） | 3,879 | 4,087 | 15,453 |

佐证（已核对的计数器，含义是推断）：
- 宿主每个 Present 有 56.3 / 58.6 / 10.4 次 GPU 提交。一次纹理提交在流打开时产生 2 次提交，27 × 2 ≈ 54，对得上。
- x86 生产端每次同步调用平均进入内核等待 1.8 / 2.4 / 5.8 次（`parks` / `syncCalls`），每个 Present 有 166 / 232 / 110 次。
  生产端先忙等 2,000 次（`d3d9_ring_channel.h:26`）才睡；几乎每次都睡，说明宿主回答一次要比忙等窗口长得多，
  这与宿主在回答前等 GPU 空一致。
- 帧记录每世界帧正好 5 条（相机、Lighting、ShadowFrame、Aa、Bloom），标记 3 条（进、出、BeforeUi）：
  8,275 / 1,655 = 5.0，4,965 / 1,655 = 3.0；宿主 `beforeUiRuns` = `aaRecorded` = `bloomRecorded` = 1,655。已核对。

顺带看到的（已核对）：固定管线批平均只有 3.7 / 4.75 / 3.85 次绘制（上限 20）。
批不是被 20 截断的，是被状态变化（Lost Temple `statesPublished` 99,829）和不能越过暂存的上传（22,699）冲断的。
这影响记录数和字节，不影响同步次数。

---

## 3. 现在的同步与晚报机制（已核对）

- **同步**：`Connection::call()`（`d3d9_connection.h:630-706`）先冲掉暂存的绘制批（656-658），
  环模式下进 `ringCall()`（846-889）：`ringPublish(sync=true)` 写一条带 `FlagSync` 的记录，
  然后 `ringAwait()`（773-810）等 `applied` 到这条的序号——先忙等 `SpinPolls=2000` 次，再在 `progress` 事件上睡，
  每 `RecheckMs=1000` 复查；滚动的无进展上限 10 秒判宿主挂死（1258）。
- **异步**：`publishUntil()`（1084-1119）在环模式下就是 `ringPublish(sync=false)`。走这条路的有
  `enqueueOrdered`（UploadBuffer / SetEffects / SetSamplerState / SetTextureStages，435-458）、
  `enqueueClear`（578-598）、`publishStaged`（绘制批与它的 AnnotateDraws，983-1009）、`contentSend`（948-958）、`pollEvent` 的 WaitEvent（603-629）。
- **晚报**：宿主执行完一条异步记录，结果不是 `D3D_OK` 就 `publishFault(AsyncRefused, hr, sequence)`，打印后退出
  （`dxvk_core/d3d9_host_main.cpp:973-989`）；`D3DERR_DEVICELOST` 单独发布为 `Fault::DeviceLost`（977）。
  x86 每个 API 入口的 `usable()` 读宿主状态（`d3d9_connection.h:719-725`），看到 Faulted 就 `ringHostStopped()`（726-735）
  → `failClosed(HostRejected, "async-refused", hr)` 或记 `HostDeviceLost`；worker 线程的 `watchRing()` 也在看宿主进程退出（907-947）。
  第一个终止原因进 `TerminalReport`，下一次 Present 时 `observePresent()` 交给数据层的 `terminal` 回调
  （`d3d9_proxy.cpp:403-416`），数据层 `ReportRenderHostLostAndExit` 让游戏退出（`war3_proxy_services.cpp:182-190`）。
- 所以“异步记录被拒 = 会话终止”今天已经成立，并已用在缓冲上传、状态、清屏、绘制批上。

---

## 4. 清单：每个调用走哪条路

列：调用 → 线上的操作 → 同步/异步 → 应用要拿回什么 → 顺序要求 → 每帧次数（Lost Temple，按 Present）→ Remix 客户端怎么做。
“本地”= 只改 x86 的状态，不发消息。代码行号是 `tools/render_host/d3d9_proxy.cpp`，另注明的除外。

### 4.1 IDirect3D9（工厂）

| 调用 | 操作 | 同步? | 应用要的 | 每帧 | Remix |
|---|---|---|---|---|---|
| GetAdapterCount / GetDeviceCaps（工厂） | FactoryQuery(Caps)，1115-1119、96-126 | 同步 | 数据 | 0（启动期） | 设备的 caps 只在创建时等一次，之后读缓存 `m_caps`：`R/client/d3d9_device.cpp:236-270` |
| GetAdapterIdentifier / ModeCount / EnumAdapterModes / DisplayMode | FactoryQuery，1120-1144 | 同步 | 数据 | 0 | 未逐个核对 |
| CheckDeviceType / Format / MultiSample / DepthStencilMatch | FactoryQuery，1149-1174 | 同步 | HRESULT | 0 | 未逐个核对 |
| CreateDevice | Initialize（管道）+ AttachRing，1249；`d3d9_connection.h:738-758` | 同步 | HRESULT | 每设备 1 次 | 握手要等 |

游戏建了三个 Direct3D9 对象，起了三个宿主；前两个只交换了 2 条和 104 条消息（`game.stderr`，已核对）。只在启动期。

### 4.2 设备生命周期

| 调用 | 操作 | 同步? | 应用要的 | 每帧 | Remix |
|---|---|---|---|---|---|
| Reset | Reset，再补发 SetEffects/SetSamplerState/SetTextureStages，1445、1459-1464 | 同步 ×4 | HRESULT（游戏检查，推断） | 0（`resets:0`） | 等回答：`R/client/d3d9_device.cpp:425-460` |
| 窗口缩放（`followWindow`） | Reset(KeepResources)，1483 | 同步 | HRESULT（拒绝就留旧尺寸） | 0（本轮无缩放） | — |
| TestCooperativeLevel / GetAvailableTextureMem / GetCreationParameters | 本地，262、267、269 | — | — | — | GetAvailableTextureMem 要等（`d3d9_device.cpp:188-191`） |
| 设备最后一次 Release | Finish，1363 | 同步 | 无，但要保证宿主收尾 | 每会话 1 次 | 设备销毁等 `Bridge_Ack`：`R/client/d3d9_device.cpp:65-80` |
| WarVKD3D9DiagnosticCheckpoint | DiagnosticCheckpoint，1388 | 同步 | 数据 | 诊断 | — |

### 4.3 创建与销毁

| 调用 | 操作 | 同步? | 应用要的 | 每帧 | Remix |
|---|---|---|---|---|---|
| CreateTexture | CreateTexture2D，1540-1549；`ResourceReservation::create` 668-680 | 同步 | HRESULT；宿主的 INVALIDCALL / OUTOFVIDEOMEMORY 可回滚预留 | LT 2,019/会话；生与死 15,791（多在载入期） | 可配置是否等，默认等：`R/client/d3d9_device.cpp:599-624`、`R/util/util_bridgecommand.h:63-73`、`R/util/config/global_options.h:344` |
| CreateVertexBuffer / IndexBuffer | CreateBuffer，1521-1531 | 同步 | HRESULT | 3/会话 | 同上 |
| CreateVertexDeclaration；SetFVF 遇到新 FVF | CreateDeclaration，1666-1684、1705-1732 | 同步 | HRESULT；SetFVF 靠 OUTOFVIDEOMEMORY 回收缓存后重试（1719-1725） | 2/会话 | 同上 |
| CreateVertexShader / PixelShader | CreateShader，1652-1663 | 同步 | HRESULT | 0（`shadersCompiled:0`） | 同上 |
| CreateQuery | CreateQuery，1685-1693 | 同步 | HRESULT | 0（`queryHandlesCreated:0`） | 不等，客户端分配 id：`R/client/d3d9_device.cpp:3305-3323` |
| CreateOffscreenPlainSurface | 本地，1550-1554 | — | — | — | 要等 |
| 资源最后一次 Release | Destroy，490-502 | 同步 | 无；但现行规则是“确认销毁后才能用下一代复用 id”（`d3d9_wire.h:8-9`） | LT 2,024/会话；生与死 15,796 | 不等：`R/client/d3d9_texture.cpp:68-70` |

资源 id 已经是 x86 分配的（`ResourceRegistry::reserve`，`d3d9_resource_registry.h:42-53`），宿主只校验同一代号（已核对）。

### 4.4 状态设置

SetRenderState / SetTextureStageState / SetSamplerState / SetTransform / MultiplyTransform / SetMaterial / SetLight / LightEnable /
SetViewport / SetScissorRect / Set*ShaderConstantF / SetStreamSource / SetIndices / SetTexture / SetVertexShader / SetPixelShader /
SetVertexDeclaration / 状态块：**全部本地**（319-364、1570-1639、1695-1756、1778-1797、`d3d9_proxy_stateblock.h` 里没有任何发送）。
绘制时比较后才发 SetEffects / SetSamplerState / SetTextureStages，有序异步（558-584、1757-1772）。
LT 每 Present 47 条。Remix：异步，并且 `client.dropRedundantState`、`client.batchState` 默认开（`R/client/client_options.h:87-97`）。

### 4.5 场景、清屏、绘制

| 调用 | 操作 | 同步? | 应用要的 | 顺序 | 每帧 | Remix |
|---|---|---|---|---|---|---|
| BeginScene | 本地，365 | — | — | — | ~1 | 异步：`R/client/d3d9_device.cpp:1109-1130` |
| EndScene | 冲暂存批（异步）+ `FlushDraws`（`STREAM=1` 时同步），366；`d3d9_connection.h:574-577` | 同步 | 无（游戏不看返回，推断） | FIFO | ≈1（推断，在余数里） | 异步：`d3d9_device.cpp:1133-1154` |
| Clear（整屏） | Clear，1839 | 异步 | 无 | FIFO，先冲暂存 | LT 2,119/会话 ≈ 1 | 异步：`d3d9_device.cpp:1157-1179` |
| Clear（带矩形或局部视口） | ClearRegions，1841-1842 | **同步** | 无 | FIFO | <1（在余数里） | 同上，异步 |
| DrawPrimitive / DrawIndexedPrimitive | DrawFixedBatch / DrawBatch（≤20）+ AnnotateDraws，1849-1910；`d3d9_connection.h:393-430、983-1009` | 异步 | 本地校验给 INVALIDCALL | 暂存；遇非绘制消息先冲 | LT 222 次绘制、60 批 | 异步 |
| DrawPrimitiveUP | 第一次或扩容时 CreateBuffer（同步）+ UploadBuffer(Discard)（异步）+ 绘制，1867-1889 | 多半异步 | — | — | 只有闪电 | 异步，并使流 0 绑定失效：`d3d9_device.cpp:2411-2431` |

### 4.6 锁与解锁

| 调用 | 操作 | 同步? | 应用要的 | 顺序 | 每帧 | Remix |
|---|---|---|---|---|---|---|
| VB/IB Lock | 本地（借窗口或驻留副本）；副本被逐出时 ReadContent，808-824 | 逐出时同步 | 数据 | — | ReadContent 62/会话 | 本地 shadow |
| VB/IB Unlock | UploadBuffer（NoOverwrite 可越过暂存，其余先冲），776-796 | 异步 | 无 | 见左 | LT 329 | 异步：`R/client/lockable_buffer.h:173-247` |
| Texture LockRect | 本地；逐出时 ReadContent，892-913、953-969 | 逐出时同步 | 数据 | — | 少 | 每个表面一份客户端 shadow：`R/client/d3d9_surface.cpp:205-237` |
| **Texture UnlockRect** | BeginTextureUpload + TextureUploadChunk × ⌈字节/65,408⌉ + CommitTextureUpload（失败时再发 Abort），914-944 | **全部同步**（`checkedCall`，855-858） | 无（游戏不看返回，推断）；x86 只用“成功”来设 `initializedLevels` / `uploaded` | 先冲暂存；在用它的绘制之前 | **LT ≈27 次提交 ≈ 81 次同步** | 完全异步，每部分 ≤16 MB、每块 1 MB：`R/client/d3d9_surface.cpp:239-334` |
| Texture SetLOD | TextureLod，878-883 | 同步 | 旧 LOD（本地就有） | FIFO | 未知（在余数里） | 未核对 |
| GetRenderTargetData；帧转储；数据层截图 | Readback，每 8 行一带（1902 宽时），444-458、1500-1519、1560-1566 | 同步 | 数据 | 在前序之后 | 诊断：484/会话 | 要等：`d3d9_device.cpp:870-886` |

### 4.7 查询

| 调用 | 操作 | 同步? | 应用要的 | 每帧 | Remix |
|---|---|---|---|---|---|
| Query::Issue | Issue，1053-1064 | 同步 | 宿主给的 serial | 0（`queryIssues:0`） | 可选地等：`R/client/d3d9_query.cpp:108-124` |
| Query::GetData | `ASYNC_EVENTS=1`：WaitEvent 异步 + GPU 水位（`d3d9_connection.h:603-629`、892-904）；否则 Poll 同步 | 异步 | BOOL | 0 | `client.asyncQueries`：客户端用共享内存里的水位回答：`d3d9_query.cpp:126-150` |

**War3 在这三张图上根本没用查询**（宿主 `queryHandlesCreated:0`、`queryIssues:0`，已核对）。

### 4.8 Present

| 调用 | 操作 | 同步? | 应用要的 | 每帧 | Remix |
|---|---|---|---|---|---|
| Present | 先是数据层的 `present` 回调（见 4.9），然后有挂起的会话重置就发 SessionReset（同步，罕见），然后 Present，371-379、417-435；`d3d9_connection.h:639-654` | 同步 | HRESULT：DEVICELOST，或 PresentSkipped（前端改报 D3D_OK）；War3 检查 Present 返回（推断） | 1 | 发出后取一个额度，`presentSemaphoreMaxFrames=1`：`R/client/d3d9_swapchain.cpp:153-198`、`R/client/d3d9_device.cpp:466-496`；服务端真 Present 之后归还：`R/server/main.cpp:739-749、1783-1810` |

宿主执行 Present 时自己也是同步的：执行前 `settle` + `waitPresentRetired`，执行后再 `settle` + `waitPresentRetired`
（`d3d9_backend.cpp:2256-2257、2321-2323`，已核对），即回答时这一帧的 GPU 工作已经做完、present 已经退役。
`reset-host` 行 `syncInterval:1`（已核对）。

### 4.9 前端导出与数据层的回调

| 入口 | 操作 | 同步? | 数据层要的 | 每帧 |
|---|---|---|---|---|
| `WarVKD3D9SetFrameScene` | FrameScene，`d3d9_connection.h:486-492` | 同步 | 成功或拒绝；一次拒绝，数据层就停发（`war3_proxy_frame.cpp:114-124`） | 每世界帧 5 |
| `WarVKD3D9MarkStage` | StageMarker，493-499 | 同步 | 同上；BeforeUi 让宿主当场跑特效（`d3d9_host_main.cpp:580-614`） | 每世界帧 3 |
| `WarVKD3D9AnnotateNextDraw` | 本地，跟着批异步发出，463-480 | — | — | LT 每 Present 267 条记录 |
| `WarVKD3D9RequestSessionReset` | 本地；下一个 Present 前发 SessionReset（同步） | — | — | 换图时 1 次 |
| `WarVKD3D9UploadContent` / `ReleaseContent` / `ReadContent` | ContentUpload / Release 异步，ContentRead 同步，517-564 | — | 数据层现在不用（`content-proxy` 全为 0） | 0 |
| `WarVKD3D9SetWorldLighting` / `SetObserver` | 本地 | — | — | — |

回调链（已核对）：
- `draw` → `OnDraw`（`war3_proxy_mode.cpp:100-103`）→ `FrameDraw`（`war3_proxy_frame.cpp:567-580`）：世界内取相机、
  `CastersDraw` 加投射体记录（本地）；世界场景之后的第一次绘制走 `InsertBeforeUi`（340-404）：5 条 FrameScene + BeforeUi 标记，全同步。
- 世界场景进出：`War3SetWorldLightingScope`（`war3_proxy_services.cpp:43-46`）→ `FrameWorldScene`（`war3_proxy_frame.cpp:545-565`）：
  WorldSceneEnter / Exit 标记，同步；`SetWorldLighting` 本地。
- `present` → `FramePresent`（582-606）：没插入过就在这里补 `InsertBeforeUi`；有截图请求时读回（`war3_proxy_services.cpp:137-144`）。

---

## 5. 同步之外的串行点：宿主的 GPU 排空（已核对代码，影响是推断）

宿主是一个线程，顺序执行环里的记录。下面这些操作在执行时等 GPU 做完、资源退役：

| 宿主操作 | 等什么 | 位置 | 次数 |
|---|---|---|---|
| CommitTextureUpload → `uploadTextureRegion` | `settle` 一次 + `submit(waitForGpu=true)` 一次 | `d3d9_backend.cpp:1408-1432` | LT ≈27 / Present |
| CreateTexture2D → `createTexture2D` | `begin`（含 `settle`）+ `submit(true)` | 1377-1401 | 生与死 15,791 次 |
| Destroy → `destroy` | `settle` + `waitPresentRetired` | 2351-2390 | 同创建数 |
| TextureLod → `setTextureLod` | `settle` | 1434-1446 | 未知 |
| Present → `present` | 前后各一次 `settle` + `waitPresentRetired` | 2221-2349 | 1 |
| Readback | `begin` + `submit(true)` | 2125-2141 | 诊断 |

意思是：x86 不等以后，宿主自己仍在每次纹理提交时停下 GPU。x86 和宿主可以重叠了，但宿主一帧的时间里
仍有几十段“GPU 跑、CPU 等”再“CPU 录、GPU 闲”的交替。**要让 P2 值回成本，宿主侧要一起改：**
上传用暂存环里的切片、把拷贝录进当前的流里（DXVK 的上下文会自己插屏障，暂存切片按命令列表的生命周期回收，
与常量环的 `invalidateBuffer` 改名是同一套做法，见 `d3d9_backend.cpp:619-629`），不再 `settle`。

---

## 6. Remix 怎么做，3f75d64 修了哪些坑

### 6.1 做法（已核对，`afb0252`）

- 只有 `WAIT_FOR_SERVER_RESPONSE` 的调用等回答；绝大多数调用用 `WAIT_FOR_OPTIONAL_SERVER_RESPONSE`，
  只有 `sendAllServerResponses`（默认 false）时才等（`R/util/util_bridgecommand.h:37-61`、`R/util/config/global_options.h:339`）。
- 创建用 `WAIT_FOR_OPTIONAL_CREATE_FUNCTION_SERVER_RESPONSE`，`sendCreateFunctionServerResponses` 默认 **true**（等）
  （`util_bridgecommand.h:63-73`、`global_options.h:344`）。8bbf141 曾给 hl2 关掉，3f75d64 又打开了：
  hl2 的应用配置现在写着 `sendCreateFunctionServerResponses=True`（`R/util/config/config.cpp:135-139`）。
- Present：先发命令，再 `syncOnPresent()` 取一个额度（`R/client/d3d9_swapchain.cpp:153-198`）；服务端真 Present 返回后
  `gpPresent->release()`（`R/server/main.cpp:1783-1810`）。信号量初值 = `presentSemaphoreMaxFrames`（默认 1），
  上限是它加 1，因为“串行化的 present 可能在客户端取下一个额度之前就做完”（`R/util/util_semaphore.h:73-76`、`global_options.h:405-410`）。
  服务端 Present 失败只记日志，客户端照样返回 D3D_OK。
- 纹理解锁：完全异步，每部分 ≤16 MB 一条命令、每块 1 MB，客户端保留每个表面一份 shadow（`R/client/d3d9_surface.cpp:205-334`）。
- 查询：事件查询在客户端用服务端发布的“最后执行的 Issue UID”回答；其他小查询发 GetData 不等，服务端写到每个查询的槽里
  （`R/client/d3d9_query.cpp:126-150`）。
- 设备 caps 创建时等一次，之后读缓存（`d3d9_device.cpp:236-270`）。
- 客户端的数据通道默认 96 MB（`global_options.h:311`），在 32 位进程里。

### 6.2 3f75d64 的坑，哪些适用于我们

| 3f75d64 修的 | 适用? | 我们怎么应对 |
|---|---|---|
| 查询：异步轮询真实 GPU 结果，保留挂起的 flush 请求，Issue / 重置与轮询在设备锁下串行 | 基本不适用 | War3 不用查询；我们是单所有者线程（`isOwner()`），天然串行；WaitEvent 同时只有一条 |
| 发送状态缓存：UP 绘制后让流 0 / 索引绑定失效；SetRenderTarget 与视口 / 裁剪缓存失效一步完成 | 今天不适用，做“固定管线状态去重”时适用 | 我们的绘制每条都带完整资源引用；已有的去重（`sentEffects` 等）在 Reset 后补发默认值（1457-1466）。以后做增量状态时，Reset、状态块 Apply、UP 绘制、窗口缩放改视口都要清缓存 |
| 上传所有权：共享堆上传做成不可变快照，服务端消费后才回收 | 今天不适用 | 环的 `tryWrite` 在发布时就把负载拷进环（`d3d9_command_ring.h:174-195`），环本身就是快照；上传窗口在发布后才还（`d3d9_proxy.cpp:793、972-974`）。将来做零拷贝时适用 |
| 环的标量和头按值返回，借来的负载用完才释放 | 已满足 | 宿主先把整条消息拷到私有 `Packet` 再校验（`d3d9_command_ring.h:269-294`）；回答也是读一次进私有内存 |
| Present 保留提前归还的额度；与窗口销毁串行；设备销毁确认前排空 Present | **适用** | §7.3：额度用单调的 `applied` 序号算，不用信号量；`~Device` 改窗口样式、卸窗口钩子之前先排空；Finish 保持同步 |
| 设备销毁要等服务端确认 | 已满足 | Finish 同步（1363） |
| 保留 Source 的创建回答（撤回异步创建） | **适用，要谨慎** | §7.5：我们的拒绝是终止，不会出现 Remix 那种“创建失败、后续上传被跳过”的悬空；但原因没写，要用生与死做浸泡测试 |
| 共享堆满时有界等待 | 已满足 | 环满就是反压，不丢、不死锁（消费端从不等生产端） |

8bbf141 的两条也已满足：`pull()` 先拷出再释放槽（我们的 `tryRead` 先 `get` 再推进 `head`），
覆盖保护比较绝对位置（我们的 head/tail 是 64 位绝对值）。

---

## 7. P2 方案

### 7.1 改成异步的，与保留同步的

改成异步（按收益排）：
1. 纹理上传三步（§7.4）。
2. FrameScene / StageMarker（需要宿主的“建议性拒绝”，§7.2）。
3. Present，领先不超过 1 帧（§7.3）。
4. EndScene 的 FlushDraws：照 `enqueueClear` 发，不等。
5. ClearRegions：同上，带负载。x86 已经裁剪并校验了矩形（1820-1835）。
6. SetLOD：有序异步。
7. Destroy：先冲暂存批（暂存的绘制可能还引用它），发出即在本地 registry 里销毁（§7.5）。
8. Create*：异步 + x86 准入镜像（§7.5）。
9. GetDeviceCaps：第一次之后读缓存（Remix 同样做法）。

保留同步：Initialize / AttachRing / Finish、FactoryQuery（启动期，可加缓存）、Reset 与缩放、Readback、
ReadContent / ContentRead、DiagnosticCheckpoint、Query::Issue（War3 不用）、SessionReset（罕见，可以以后再改）。

做完以后，稳态帧里剩下的同步只有：逐出后的 ReadContent、截图读回、Reset。LT 每 Present 从 92 降到约 0–1（推断）。

### 7.2 错误怎么晚报

沿用 §3 的链，不另造机制。要补的：

1. **宿主把“软结果”当成功。** 今天宿主对异步记录的规则是“不是 D3D_OK 就拒绝”（`d3d9_host_main.cpp:976`）。
   - Present 返回 `PresentSkipped`（窗口已缩放、等 Reset，`d3d9_wire.h:131-134`）：异步时必须记为成功并计数，否则一次缩放就终止会话。
   - FrameScene / StageMarker 的场景拒绝（例如 `canInsertBeforeUi()` 为假，`d3d9_host_main.cpp:585`）：
     今天是可恢复的——数据层停发，游戏画面照常（`war3_proxy_frame.cpp:112-124`）。异步后要么宿主返回成功、
     在消费端控制块里置一个“场景记录被拒”标志（`ConsumerBlock` 偏移 116 的 `reserved1[12]` 或 152 的 `reserved3[40]` 有空，
     `d3d9_command_ring.h:58-66`），x86 在下一次 Present 读到后照旧停发；要么接受“拒绝 = 终止”（x86 已经镜像了宿主的
     SceneTracker，按设计不该被拒）。这是作者要定的产品取舍：前者保留“特效坏了画面还在”，后者更简单。
2. **x86 预判今天可以回滚的失败**：创建的 INVALIDCALL / OUTOFVIDEOMEMORY（§7.5）、SetFVF 的声明回收、纹理上传的版本号与区域
   （x86 已经有 `revision`、`MakeManagedLockRegion`、首次整层上传的规则，914-924）。预判不了的（真的显存不够）今天就是 DEVICELOST，不变。
3. **终止报告写出被拒的是哪条。** 宿主已发布 `faultSequence`；x86 留最近 64 条（序号、操作、id），
   终止时把对应的操作名写进 `TerminalReport.detail`。

到达时延：最多一帧——下一次 API 调用的 `usable()`，或 Present 等额度时 `ringAwait` 看到 Faulted。
游戏看到的是 `D3DERR_DEVICELOST`，然后由 `terminal` 回调退出，终点与今天同步拒绝时相同。
代价：被拒的那条之后 x86 已经提交了一些本地状态（预留、`uploaded` 标志），但会话已经终止，`~Device` 走的是失败路径，无影响。

### 7.3 Present：领先不超过 1 帧

- x86 记每个 Present 记录的序号。Present N 异步发出；返回前等 `applied ≥ seq(Present N−1)`（复用 `ringAwait`，
  有 10 秒无进展判挂）。即 x86 做第 N+1 帧时，宿主最多还在做第 N 帧，与 `presentSemaphoreMaxFrames=1` 等价。
  用单调序号就不需要信号量，也就没有 Remix 为“提前归还的额度”把上限设成 frames+1 的问题。
- 不需要新的线上字段：`applied` 已经在消费端控制块里，宿主在生产端等待时会 `SetEvent(progress)`（`d3d9_host_main.cpp:984`）。
- `IsolatedPresenterReadback` 剖面要拿回图像，保持同步（424-434）。
- 排空点（对应 3f75d64）：`~Device` 在 `leaveFullscreen()` / `unhookWindow()` 之前（1327-1328）、
  Reset 里改窗口样式之前（1430-1434）先等 `applied ≥ 最后一个 Present`。Finish 本来就同步，在 FIFO 末尾。
  `resizeToWindow` 发的是同步 Reset，FIFO 保证它在前面的 Present 之后执行。
- 宿主侧：PresentSkipped 记成功（§7.2）；DEVICELOST 已有 `Fault::DeviceLost`。
- 收益的来源：宿主的 Present 自己要等 GPU 做完这一帧、present 退役（§4.8），这段时间里 x86 已经在做下一帧。
  宿主去掉 Present 里的等待（让 DXVK 自己留 1 帧在飞）是另一步（H4）。

### 7.4 纹理上传：完全异步，Commit 也不要回答

- 三步都走 `enqueueOrdered`（不越过暂存，先冲批），把三个操作加进 `AsyncOrderedOp`（`d3d9_connection.h:435-437`）——
  管道模式下 worker 的 `acceptReply` 也靠它判断哪些是异步（1124-1129）。
- 发布即 `++revision`、置 `initializedLevels`、`uploaded`。宿主按 FIFO 先执行上传再执行用它的绘制，x86 的本地检查与宿主一致。
- 去掉 Abort：不再有逐块的失败返回；任何拒绝都是终止。
- 为什么不只让 Commit 回答：x86 什么都不需要拿回来，Commit 回答只是让错误早到一点，却每次提交仍要一次往返
  （LT 每 Present 27 次）。Remix 也是完全异步。
- 可选：补丁 ≤64 KiB 时（LT 平均约 3 KB，推断）合成一条 `TextureUpload` 记录，三条变一条。这只减记录数，不减同步数，可以放后面。
- 不需要 x86 副本：负载在发布时已拷进环。Remix 的每表面 shadow 正是我们用上传窗口和驻留预算避开的 x86 内存。
- 宿主侧 H1（§5）不做的话，宿主仍每次提交排空 GPU 两次。

### 7.5 创建与销毁

- **id 已经在 x86 分配**（`ResourceRegistry::reserve`）。异步创建 = 发出即 `commitReservation`。
- **准入镜像**（x86 要预判宿主今天可回滚的拒绝）：
  - 格式：宿主每次 CreateTexture2D 都用工厂查一遍格式（`d3d9_host_main.cpp:409-412`）；x86 每种格式第一次见到时同步查一次 CheckDeviceFormat，缓存。
  - 槽位：宿主 `BufferSlots=8`（VB 与 IB 共用）、`ShaderSlots=4`、`DeclarationSlots=4`、`TextureSlots=16,320`（`dxvk_core/d3d9_backend.h:49`）。
    x86 按类计活的资源，满了就本地返回 OUTOFVIDEOMEMORY；SetFVF 的回收逻辑改成看本地计数。
  - 真的显存不足：今天就是 quarantine → DEVICELOST，不变。
- **销毁**：FIFO 下，Destroy 在后面任何同 id 新一代的 Create 之前执行，宿主 `canCreate` 照样只认下一代；
  被拒就终止，不会有复用。所以“确认销毁后才复用”可以改写成“排在前面，且任何拒绝都终止”，
  同时改 `d3d9_wire.h:8-9` 与 `d3d9_resource_registry.h:21-24` 的说明。必须先冲暂存批。
- **注意**：生与死一局建了 15,791 张纹理，`TextureSlots` 是 16,320；活着的峰值没有记录（只有 `logicalHandlesReused:62`）。
  离上限可能不远，要先量（§10）。
- **注意**：Remix 为 HL2 撤回了异步创建（§6.1），原因没写。我们的失败模型不同，但仍要浸泡测试。

### 7.6 查询

War3 不用。保留现在的 `ASYNC_EVENTS` 路径，Issue 仍同步。将来要用时：Issue 异步，x86 用这条记录的序号当令牌，
宿主多发布一个“GPU 已完成的记录序号”，x86 本地回答 GetData（Remix 的做法）。这要在消费端控制块加字段，不进 P2。

### 7.7 环要多大

数据（已核对计数器，每帧字节是推断）：

| | Lost Temple | EchoIsles | 生与死 |
|---|---|---|---|
| 环字节 | 2.55 GB | 3.60 GB | 7.91 GB |
| 每 Present | 1.21 MB | 1.60 MB | 0.82 MB |
| 每世界帧（上限，含载入期） | 1.54 MB | 1.99 MB | 12.4 MB |
| 固定管线绘制记录（每条 3,180 B）占环字节 | 59% | 63% | 49% |
| 环满等待 | 0 | 0 | 38,118 次、12.7 秒 |

- 任务书的“6.4 MB/帧”是 7.91 GB ÷ 1,240。按世界帧算，生与死游戏内每帧在 6–12 MB 之间（进图前那段的字节没法拆出来）。
  所以 16 MiB 约合生与死 1.3–2.6 帧，不是 2.5 帧；而这轮门实际用的是 4 MiB。
- 要多大：消费端从不等生产端，环满只是反压，不会死锁。环需要装下“宿主卡在 Present 里时，x86 写出的字节”。
  宿主的 Present 要等 GPU 做完这一帧再等 present 退役（§4.8），大约一个 GPU 帧；x86 在这段时间里最多写下一整帧。
  LT / EI 一帧 1.5–2 MB，4 MiB 够；生与死 6–12 MB，**16 MiB 是下限，32 MiB 才宽裕**。
- 代价：生产端区域在 x86 地址空间里，16 MiB 比现在多 12 MiB，32 MiB 多 28 MiB——与目标 1（x86 内存）冲突。
  Remix 的 96 MB 通道不是我们的参照。`WARVK_D3D9_COMMAND_RING_KIB` 已经能配（`d3d9_connection.h:87-94`，2 的幂，256–65,536 KiB）。
- 真正的杠杆是字节：固定管线绘制记录占一半到三分之二。路线图里的“固定管线状态去重”把每条 3,180 B 换成增量后，
  生与死大概 4–8 MiB 就够了（推断）。
- 已有的风险，不是 P2 新增的：游戏的 D3D 线程就是窗口线程；x86 在环上等时不抽消息。宿主若有需要该窗口线程处理消息的同步操作就会卡住，直到超时。今天同步调用时也一样。

---

## 8. 分类、收益、工作量

“每 Present 去掉的同步”按 Lost Temple / 生与死；生与死的数字是按 Present 平均（被载入期摊薄）。

| # | 改动 | 谁做 | 去掉的同步（LT / 生与死） | 工作量 |
|---|---|---|---|---|
| P2-0 | 计数与计时：x86 按操作记同步次数与等待时间（QPC 包住 `ringAwait`），每 Present 的环字节（p50/p95/max）；宿主在环模式下按操作记执行时间、`settle` / `waitPresentRetired` 的等待时间、活资源峰值 | Opus（机械） | 0 | 0.5 天 |
| P2-1 | 纹理上传三步改有序异步，去掉 Abort | Opus（晚报机制现成，x86 已镜像宿主规则） | ≈81 / ≈12 | 1 天（含 CPU 测试） |
| P2-2 | EndScene 的 FlushDraws 异步 | Opus | ≈1 / ≈1 | 0.25 天 |
| P2-3 | ClearRegions 异步 | Opus | <1 / <1 | 0.25 天 |
| P2-4 | SetLOD 异步 | Opus | 未知（余数内） | 0.25 天 |
| P2-5 | GetDeviceCaps 缓存 | Opus | 未知，可能为 0 | 0.1 天 |
| P2-6 | Destroy 异步（先冲批；改不变式说明；CPU 测试“销毁后同 id 下一代创建”） | Opus | ≈1 / ≈1.6（集中在载入与收尾） | 0.5 天 |
| P2-7 | Create* 异步 + 准入镜像（格式缓存、槽位计数、SetFVF 回收改本地） | **Fable**（今天可回滚的失败变成必须预判，否则终止） | ≈1 / ≈1.6（载入期） | 1.5–2 天 |
| P2-8 | FrameScene / StageMarker 异步 + 建议性拒绝标志 | **Fable**（消费端控制块加字段，可恢复失败的语义要改） | ≈6.3 / ≈0.5（每世界帧 8） | 1–1.5 天 |
| P2-9 | Present 领先 ≤1 帧；排空点；宿主 PresentSkipped 记成功 | **Fable**（Present 错误晚一帧报；窗口收尾顺序） | 1 → 只在落后超过 1 帧时等 | 2 天 |
| P2-10 | 环默认改 16 MiB（只是配置） | Opus 改，**作者定**（x86 地址空间的取舍） | 0；生与死环满时间应下降 | 0.1 天 + 量 |

宿主侧（不是同步调用，但决定 P2 有没有效果）：

| # | 改动 | 谁做 | 去掉的 | 工作量 |
|---|---|---|---|---|
| H1 | 纹理上传不 `settle`：暂存环切片 + 录进当前流 | Opus（DXVK 现成的做法），要过 GPU 门与画面对照 | LT 每 Present 约 54 次 GPU 排空（推断） | 1.5–2 天 |
| H2 | CreateTexture2D 的 `initImage` 录进流，不 `settle` | Opus | 生与死载入期 15,791 次排空 | 0.5 天 |
| H3 | SetLOD 不 `settle`（新视图替换，旧视图由命令列表持有） | Opus | 未知 | 0.25 天 |
| H4 | Present 里不等 GPU 和 present 退役，让 DXVK 留 1 帧在飞 | **Fable**（WSI、窗口收尾、帧号诊断的语义） | 每帧一次完整排空 | 2 天 |
| H5 | Destroy 不 `settle`（靠 DXVK 的引用计数保活） | **Fable**（“退役后才墓碑”的不变式） | 每次销毁一次排空 | 1 天 |
| X | 固定管线绘制记录改增量（路线图已有） | 线格式两端都改；Opus 可做，Fable 审 | 0 同步；环字节 −49% 到 −63% | 2–3 天 |

---

## 9. 给计划的结论

1. **先做 P2-0（Opus，半天）。** 现在每个操作的同步次数是从计数器反推的，帧时间里 x86 计算、x86 等待、宿主执行、
   宿主等 GPU 各占多少完全没有数。没有这组数，就不知道 P2 之后瓶颈是不是移到了宿主。
2. **然后 Opus 一批做 P2-1 到 P2-6，加 H1 到 H3。** 这一批去掉 LT / EI 每 Present 约 84 次同步（92 次里的九成多），
   以及宿主每帧约 54 次 GPU 排空。不改线上字段，不改可恢复失败的语义（P2-1 的拒绝今天就该是合同错误）。约 5 天。
   跑一轮 A/B 门（同一轮里比两臂），看帧率与 P2-0 的分解。
3. **Fable 回来后做 P2-9、P2-8、P2-7，加 H4、H5。** 它们改的是“错误什么时候、以什么形式到达”和窗口收尾顺序。
   P2-9 要等 1、2 完成后才看得出收益（今天 Present 只占 1% 的同步）。约 8–9 天。
4. **环的大小先不改默认值。** 门里给生与死单独配 16 MiB 量一次；长期靠 X（绘制记录增量）把字节砍半，而不是把环做大。
5. 不做：查询模拟（War3 不用）；Reset、Readback、ReadContent 异步化（要数据或罕见）。

---

## 10. 还不知道的

| 问题 | 怎么量 |
|---|---|
| 每个操作的真实同步次数：EndScene 每帧几次、ClearRegions、SetLOD、GetDeviceCaps 各多少（现在都在“余数”里） | P2-0：x86 按 `Op` 计 `ringCall` 次数 |
| 帧时间的分解：x86 计算 / x86 等回答 / x86 等环 / 宿主执行 / 宿主等 GPU / 宿主等 present 退役 | P2-0：x86 用 QPC 包住 `ringAwait`；宿主用 QPC 包住 `apply`（按操作）和 `settle`、`waitPresentRetired` |
| 每帧 27 次纹理提交是哪些纹理（尺寸、格式、层、矩形、频率）。若是同一张动态纹理（迷雾、小地图、字形缓存），也许能合并同帧多次提交 | 前端按纹理记 UnlockRect 直方图，每 900 帧打一行 |
| 帧率像是被垂直同步量化了：代理 29.1 / 30.3 / 9.8 fps 接近 60 Hz 的 1/2 和 1/6；宿主 `syncInterval:1`，Present 等 present 退役。进程内臂可能开着“解锁帧率”（`src/d3d9/d3d9_swapchain.cpp:490-491`），代理模式还没有这个开关（`war3_proxy_frame.cpp:82-84`）。如果属实，A/B 门比的不是同一件事（推断） | 代理臂用 `D3DPRESENT_INTERVAL_IMMEDIATE` 跑一次；记录隔离桌面的刷新率；进程内臂记录实际 present 间隔 |
| 生与死游戏内每帧的环字节（6–12 MB 之间） | P2-0：x86 每 Present 记发布字节 |
| 生与死活纹理的峰值与 `TextureSlots=16,320` 差多少 | 宿主记 `registry.liveCount()` 的峰值 |
| Remix 为什么对 HL2 撤回异步创建 | 读 3f75d64 的测试（`test/rtx/unit` 下 sent state、shared heap）与相关 issue；我们自己用生与死做异步创建的浸泡测试 |
| 游戏到底看不看 UnlockRect / EndScene / Clear / Create* / Present 的返回值（表里标了推断） | 前端失败注入后看游戏行为，或查 `docs/research/ida_export` |
| 异步以后宿主会不会成为瓶颈（宿主 `parks` 每 Present 只有约 9 次，说明它多数时间在忙） | P2-0 的宿主执行时间与空闲时间 |
| 隔离桌面上的 present 节奏是否与玩家桌面一致 | 在玩家桌面跑一次代理模式（作者环境） |

---

## 引用

本仓库路径相对 `E:\Mycode\Source\Repos\War3MapReforge\Core\Base\Graphics\dxvk`（工作树当前状态，含未提交改动）：
`tools/render_host/d3d9_proxy.cpp`、`d3d9_connection.h`、`d3d9_command_ring.h`、`d3d9_ring_channel.h`、`d3d9_wire.h`、
`d3d9_resource_registry.h`、`dxvk_core/d3d9_host_main.cpp`、`dxvk_core/d3d9_backend.cpp`、`dxvk_core/d3d9_backend.h`、
`src/d3d9/war3/platform/war3_proxy_frame.cpp`、`war3_proxy_mode.cpp`、`war3_proxy_services.cpp`、`war3_proxy_casters.cpp`。

Remix（dxvk-remix `afb025228646328613a6d234e2202517ea75f739`，`R` = `https://github.com/NVIDIAGameWorks/dxvk-remix/blob/afb025228646328613a6d234e2202517ea75f739/bridge/src`）：
- Present：[client/d3d9_swapchain.cpp#L153-L198](https://github.com/NVIDIAGameWorks/dxvk-remix/blob/afb025228646328613a6d234e2202517ea75f739/bridge/src/client/d3d9_swapchain.cpp#L153-L198)、
  [client/d3d9_device.cpp#L466-L520](https://github.com/NVIDIAGameWorks/dxvk-remix/blob/afb025228646328613a6d234e2202517ea75f739/bridge/src/client/d3d9_device.cpp#L466-L520)、
  [server/main.cpp#L1783-L1810](https://github.com/NVIDIAGameWorks/dxvk-remix/blob/afb025228646328613a6d234e2202517ea75f739/bridge/src/server/main.cpp#L1783-L1810)、
  [util/util_semaphore.h#L73-L76](https://github.com/NVIDIAGameWorks/dxvk-remix/blob/afb025228646328613a6d234e2202517ea75f739/bridge/src/util/util_semaphore.h#L73-L76)
- 等待宏：[util/util_bridgecommand.h#L37-L73](https://github.com/NVIDIAGameWorks/dxvk-remix/blob/afb025228646328613a6d234e2202517ea75f739/bridge/src/util/util_bridgecommand.h#L37-L73)
- 选项：[util/config/global_options.h#L305-L422](https://github.com/NVIDIAGameWorks/dxvk-remix/blob/afb025228646328613a6d234e2202517ea75f739/bridge/src/util/config/global_options.h#L305-L422)、
  [client/client_options.h#L87-L97](https://github.com/NVIDIAGameWorks/dxvk-remix/blob/afb025228646328613a6d234e2202517ea75f739/bridge/src/client/client_options.h#L87-L97)、
  [util/config/config.cpp#L135-L139](https://github.com/NVIDIAGameWorks/dxvk-remix/blob/afb025228646328613a6d234e2202517ea75f739/bridge/src/util/config/config.cpp#L135-L139)
- 设备：[client/d3d9_device.cpp#L65-L80](https://github.com/NVIDIAGameWorks/dxvk-remix/blob/afb025228646328613a6d234e2202517ea75f739/bridge/src/client/d3d9_device.cpp#L65-L80)（销毁确认）、
  [#L236-L270](https://github.com/NVIDIAGameWorks/dxvk-remix/blob/afb025228646328613a6d234e2202517ea75f739/bridge/src/client/d3d9_device.cpp#L236-L270)（caps）、
  [#L599-L624](https://github.com/NVIDIAGameWorks/dxvk-remix/blob/afb025228646328613a6d234e2202517ea75f739/bridge/src/client/d3d9_device.cpp#L599-L624)（CreateTexture）、
  [#L1109-L1179](https://github.com/NVIDIAGameWorks/dxvk-remix/blob/afb025228646328613a6d234e2202517ea75f739/bridge/src/client/d3d9_device.cpp#L1109-L1179)（Begin/EndScene、Clear）、
  [#L2411-L2457](https://github.com/NVIDIAGameWorks/dxvk-remix/blob/afb025228646328613a6d234e2202517ea75f739/bridge/src/client/d3d9_device.cpp#L2411-L2457)（UP 绘制）、
  [#L3305-L3323](https://github.com/NVIDIAGameWorks/dxvk-remix/blob/afb025228646328613a6d234e2202517ea75f739/bridge/src/client/d3d9_device.cpp#L3305-L3323)（CreateQuery）
- 上传：[client/d3d9_surface.cpp#L205-L334](https://github.com/NVIDIAGameWorks/dxvk-remix/blob/afb025228646328613a6d234e2202517ea75f739/bridge/src/client/d3d9_surface.cpp#L205-L334)、
  [client/lockable_buffer.h#L173-L247](https://github.com/NVIDIAGameWorks/dxvk-remix/blob/afb025228646328613a6d234e2202517ea75f739/bridge/src/client/lockable_buffer.h#L173-L247)、
  [client/d3d9_texture.cpp#L68-L70](https://github.com/NVIDIAGameWorks/dxvk-remix/blob/afb025228646328613a6d234e2202517ea75f739/bridge/src/client/d3d9_texture.cpp#L68-L70)
- 查询：[client/d3d9_query.cpp#L108-L176](https://github.com/NVIDIAGameWorks/dxvk-remix/blob/afb025228646328613a6d234e2202517ea75f739/bridge/src/client/d3d9_query.cpp#L108-L176)
- 提交：[8bbf141](https://github.com/NVIDIAGameWorks/dxvk-remix/commit/8bbf141)（“Bridge: cut per-frame IPC cost of the D3D9 bridge”）、
  [3f75d64](https://github.com/NVIDIAGameWorks/dxvk-remix/commit/3f75d64)（“Bridge: preserve D3D9 synchronization and bound shared upload memory”）。
  这两个提交的说明读自 GitHub 提交页；3f75d64 的页面只展开了 5 个客户端文件的差异，其余文件只读到提交说明。
