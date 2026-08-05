# 蒙皮源数据、线程与生命周期边界

## 1. 原生可证事实与工程安全合同必须分开

静态 ASM 已证明同一嵌套调用链中的 producer、queue flush、dynamic upload、CPU skin kernel、
Unlock 和 DIP 顺序。新增 `_beginthreadex` xref 已确认 Game.dll 创建
`MainLoop_6F05F710` 的 EvtSched Engine workers；但 registered callback/indirect dispatch 仍未
证明哪一个 OS thread 拥有某次 RenderQueue upload，也未证明固定 `renderThreadId` 或原生异步
worker join。项目实现中的 TLS、epoch、mutex、lease 和 poison ledger 是安全设计，不能反向
当成 Game.dll 的原生线程事实。

## 2. 最早 producer 与权威 stage 窗口

stage/tag 的最早可证 producer 边界为：

```text
CWorldFrameWar3::RenderScene
  -> DispatchStage(stageId,...)
     -> TerrainShadow producer and/or RenderWorldGroup(groupIdx)
        -> WorldGroupRecord[+0] strong CSprite ref
        -> CSprite_PrepareAndQueueAttachedRenderObject
           -> RenderQueue_AddBatch
```

普通 queue record 和 `RenderQueue_Dispatch_Common` ABI 都不带 CWorld stage/tag。stage 11 还先
运行 TerrainShadow selector 12，再运行 group 0。因此：

- stage 应在 producer 执行期间记录，不能到 flush 时重建；
- `WorldObjects` 的最窄身份是 `RenderWorldGroup(groupIdx=0)` 的 before/after queue range；
- `stageId==11` 只能证明 stage，不能证明 record 来自 group 0；
- group 0 的内部 gameplay 类别仍是 Unknown，不能直接写成 doodad/unit/building。

三个 owner 的 add/clear 合同还给出更早的对象边界：`0x6F0CB110` acquire `CSprite*` 后保存到
record `+0`，`0x6F0CAB90` 在帧尾逐项 release。release 归零可同步析构并回池；clear 后 record
存储可复用。因此 producer sidecar 若暂存身份，最晚必须在 clear 前完成所需复制/关联，不能把
record 地址或旧 sprite 指针跨 clear 继续使用。add 是先增加 activeCount、再写 record，故该
计数不具备 lock-free publish 语义；只能在 add 正常返回后的现有串行调用窗口消费。

## 3. CPU/GPU 蒙皮源数据的不可变窗口

真实 native 链：

```text
RenderQueue_FlushSortedItems
  -> Common/Special dispatch
  -> RenderQueue_ApplyDrawStateAndSamplerPair
  -> GxDevice_UploadDynamicVertices wrapper
  -> CGxDeviceD3d_UploadDynamicVertices @ RVA 0x0EEA50
     -> LockDynamicVertexRing @ RVA 0x0EE5D0
     -> SkinCopyVerticesToMappedVB @ RVA 0x0EDDC0
     -> Unlock / SetFVF / SetStreamSource
  -> index upload/bind
  -> one or more actual DIP
```

在 kernel gate 可以授权跳过 CPU 写循环前，必须对同一 job 重验：

- position、normal、group-slot 源字节；
- format 2/4 的 UV 字节；
- output format、stride、FVF、vertex count；
- palette pointer、group count 和 `groupCount*48` live bytes；
- 每个 group slot 均小于 group count；
- geoset primitive/index bytes 与静态 GPU resource；
- map/device/frame/batch/token/offset/length/usage 的 exact lease identity；
- Main/Shadow/Outline consumer reservation 状态。

这组 bitwise live recheck 定义了可用于 GPU 的不可变快照窗口。hash、旧缓存、同地址指针或
`CWorldObjects` 容器仍存活都不能替代该窗口。

CPU-MT Phase B 的更窄 native evidence package 位于
[cpu_mt_phase_b_native_evidence.md](../28_gpu_skinning_takeover_feasibility/cpu_mt_phase_b_native_evidence.md)：
palette/static inputs 最早在 `0x6F13A5C5/0x6F138F55` 一侧冻结，而 output identity 直到
`0x6F0EEB76` 的 successful Lock 后才存在。两段必须以 exact candidate token 关联；现有
diagnostics-only Lock sidecar 没有 source/palette/job token，ABI 9 也没有 CPU submit/acquire/
commit/abort 回调。worker output 只能在 kernel detour non-blocking acquire，并在
`0x6F0EEB8A` 前完成 owner copy/join；normal/fault 已合流的 `0x6F0EEBA6` 和 Unlock
`0x6F0EEBB6` 都太晚。

pose/palette 的更早一层 producer 来自 `CModel`，不是 `CWorldFrameWar3`：现有 ASM 计划把
`RVA 0x12FDC0` 识别为按 `CModel+0x5C` count 向 `CModel+0x60` 输出 48-byte palette 的最终写入，
并把 `RVA 0x12F7E0` 返回后视为 root/child/attachment tree 的第一稳定点。该点当前是进一步审计
入口；不能因为 WorldFrame 驱动帧更新，就把 WorldFrame 写成 palette authority。

## 4. 三个提交边界

native upload 的 ASM 给出三个不能混用的边界：

1. **可逆窗口**：Lock 已返回真实 mapped pointer，kernel detour 尚未返回。任何 preflight
   失败都仍可 exactly-once 调原 kernel，并留在 caller-owned SEH 内。
2. **逻辑 commit**：detour 返回且没有调用原 kernel。此后 native VB slice 已在逻辑上是 stale，
   必须登记 poison/consumer ledger，不能再选择 CPU fallback。
3. **物理不可逆点**：`RVA 0x0EEBB6` 对 VB Unlock。mapped pointer 失效；之后即便 index、
   stream 或 DIP signature 不匹配，也不存在合法的晚 CPU rescue，只能 suppress + fuse。

caller-owned SEH 还要求区分“选择 original”“进入 original”“normal return”和“exact range 已
覆写”。只有 trampoline 正常返回后的 exact identity/range ack 可以退休 CPU overwrite poison；
fault 被 outer caller handler 吸收的路径没有 normal-return ack。

## 5. 最晚 join/consumer 点

一次 vertex upload 不天然对应下一个 DIP：index immediate flush、Common tail flush、special/
multipass slice 均允许 0/1/N fan-out。consumer 必须按以下 mutable tuple 关联：

```text
dispatch/flush scope + upload epoch + DIP ordinal
+ output format/FVF/stride/stream identity
+ vertexCount/native vertex base
+ primitiveCode/indexCount/StartIndex/BaseVertexIndex
+ actual PrimitiveType/NumVertices/PrimitiveCount
```

主画面 DIP 的 exact consumption 可在 dispatch end 核销；真正的 CSM/point-shadow 或 outline
consumer 可能晚于该点，所需 GPU output lease 必须保留到对应 frame retirement，不能在
`DispatchStage`、flush 或主 DIP 后过早释放。

因此当前可证/安全的“最晚 join”不是单一 Game.dll 函数：

- Main：exact actual DIP consumer；
- Outline/Shadow：各自 reservation 被实际消费或明确 `NotRequested`；
- resource：frame retirement 与 exact resource-generation retirement；
- reset/unload：全部 transaction quiescent、outstanding poison 归零、owner ack exact match。

## 6. 对象销毁、resource retirement 与 reset

### 6.1 Game.dll 对象层

- `CGameUI` 拥有 `CWorldFrameWar3* @ +0x3BC`；WorldFrame 销毁会遍历三组
  `WorldGroupRecordOwner` 的全部 constructed records、release 每个 `CSprite*`，再释放
  backing/owner 及其他 embedded owner。Mini/Uber pooled leaf 的 release-to-zero 会先以
  flags=0 调 slot0，再回到各自 pool；这是真实对象 stop-use 边界。
- WorldFrame base destruction 在清理 inherited `+0x30/+0x2C` 之前，经 primary slot27 调
  `CWorldFrameWar3_VirtualTeardownResources @ 0x6F367B40`。该 override 会释放
  `CFog* @ +0x334`、pooled `CSpriteUber_` leaf refs `+0x338/+0x33C/+0x354`、base
  `CLight* @ +0x340`、`TargetIndicatorVector @ +0x370` 的每个 record strong ref、
  `+0x588/+0x58C` 以及 `RallyIndicatorSmallVector16 @ +0x3B0` 中每个 `runtimeObject08`；
  因此这是已证的对象层 stop-use/teardown 边界，但仍不能代替 DXVK resource-generation
  retirement ack。`+0x378` 是 vector data pointer，不是 inline/fixed “8 handles owner”。
- `CPathingMapIndicatorRefVector @ +0x360` reallocate 会先为新 backing acquire refs、再 release
  旧 backing refs；destroy 逐 count release 后 free。`TargetIndicatorVector @ +0x370` 也可
  reallocate，record `+0x04` 是 strong ref。两者的 `data`、record address、element pointer 都
  没有跨 grow/destroy 的稳定 identity 合同；初始 count 8 和 cursor `&7` 只定义当前 ring 使用法。
- stage-0 `+0x354` 的 producer 会替换 strong pooled-Uber ref，`+0x35C` 只是当前 update 的
  prepare/visibility result；`+0x358/+0x35C` render gate 不冻结 sprite/model/palette，也不延长
  `+0x354` 的引用生命周期。环境 `CFog/CLight` 与两个 DNC sprite 同理属于 WorldFrame owner，
  不是 D3D resource retirement authority。
- `+0x3B0` small vector 和 `+0x590` Waypoint vector 拥有各自 backing，并 release record 内的
  runtime-object refs；`+0x5A0..+0x5F0` 六个 raw-pointer vectors 只拥有 backing、借用元素；
  `+0x600..+0x650` 六个 `CAgentPtr<T>` vectors 同时拥有 backing 与元素 refs。跨 clear/dtor
  保存 data/record/element 指针均无稳定性合同；`CCaptainAI*` count 未被帧尾 helper 清除也不
  代表可跨线程长期借用，其保留原因仍 Unknown。
- embedded `CCinematicFilter @ +0x254` 在 WorldFrame dtor 中以 non-deleting dtor 销毁并释放其
  RCString/owned buffers；唯一虚槽的 flags-bit0 free 只适用于独立 scalar-delete 调用，不能对
  `this+0x254` 使用。`+0x178/+0x184/+0x188/+0x230/+0x32C` 是不 acquire 的 borrowed
  game-context publications，读者必须服从当前 owner refresh/teardown 顺序。
- `CLayoutFrame@+0xB4` 的 rect/scale mutation、hover enter/leave 和 `AUKeyboardFocus` attach/
  detach 都属于 UI frame 生命周期；当前没有 xref 证明它们会稳定或冻结 skin source，不能把
  这些 slot 当成 GPU skin producer/join/reset authority。
- `CDoodads` 是独立全局单例对象；其类对象、`CWorldObjects` 基类和 `0x188`-stride model
  entries 是三种生命周期，禁止混为一个 `0x188` 类对象。
- `CDoodads_Dtor @ 0x6F74BEB0` 在 base dtor 前调用 `0x6F750FC0`；后者先以
  `0x6F751770` 对每个 entry 做 pre-dtor cleanup，再调用 `AUWOModel_Dtor` 并释放 array。
  该静态 dtor 链明确注销 selection-circle handle `entry+0x8C`，但没有直接注销
  `+0x88` static-shadow、`+0x90` emitter 或 `+0x94` auxiliary terrain handle；三者只在已审的
  explicit disable/destroy 路径结算。因此“进入 AUWOModel/CDoodads dtor 就自动完成全部
  CTerrain resource retirement”被否定，外层 teardown 次序/CTerrain owner 兜底仍是 Unknown。
- `CWorldObjectsClippable::Clone` 分配独立 `0xB4`-stride outer array，并 copy-construct 已证
  nested owners；record raw 区仍原值复制，可能保留 borrowed/shared references。enum-like
  `+0x04` kind/mode 与 `+0x18` state 也是值复制。因此不能从“outer array 独立”推出所有
  pointee 跨线程不可别名。
- `CTerrain+0x1F0` owns `CBlightPuffs`；publish 发生在 ctor 完成后，teardown 以 vslot0 flags=1
  销毁并清 pointer。更新链会写 AUWOModel active flag、elapsed/duration/lifecycle；静态 ASM
  只证明当前嵌套链的先后，没有锁、worker handoff 或与 RenderQueue 并发的授权证据。因此 GPU
  skin snapshot 仍须在 producer-side current frame capture 后按既有 immutable window 使用，
  不能因 `CBlightPuffs` 对象由 CTerrain owns 就把 entry 视为跨线程 immutable。

这些事实只证明容器内存生命周期，不能证明 D3D/Vulkan resource 已退休。

### 6.2 DXVK output/resource 层

安全身份至少为：

```text
PoisonIdentity = (D3D9CommonBuffer*, identityGeneration)
PoisonRange    = identity + interval + stride/FVF/format
ResetOwnerAck  = exact(ownerIdentity, resetGeneration)
```

- CPU overwrite 只可扣除 exact same identity/layout 的实际覆盖区间；中间覆盖需 split。
- `DISCARD`、format replacement、owner detach、COM pointer 重用或普通 reset 都不是 resource
  destructor ack。
- 只有 exact common-resource generation 的 destructor/retirement event 可一次性退休余下 ranges。
- reset request 生成 `R` 后立即停发新 authorization；完成需同一 owner 的 `ackGeneration==R`、
  TLS/global transaction quiescence 和 outstanding poison 已安全归零。
- map/device epoch 改变可以清 authorization/lease 并提升 generation，但不能清仍可调用的
  trampoline，也不能把旧 poison 静默视为已退休。

## 7. Hook 生命周期

三个 native hook 的安装事务为 apply draw-state -> outer upload -> kernel；全部成功后才可发布
bridge enabled。卸载顺序相反，并且必须先停止授权、等待 active detour/TLS/ledger 排空。
partial rollback 只能移除本事务实际拥有的 hook；任何仍可能被调用的 detour 都要求保留原
trampoline 和模块 lifetime。

这些是根据 Game.dll ABI 得出的工程 hard gate，不是对原生类继承或线程模型的描述。

## 8. 仍为 Unknown

新增 stage 证据把对象发布窗口进一步收窄，但不把“同步调用”外推成“全引擎单线程”：

- stage16 四个 `CUnit` bucket、stage18 build visuals、stage21 两个 TextTag/TerrainImage pass 都在
  `CWorldFrameWar3_DispatchStage` 当前调用栈中同步完成；这些函数及其已审 caller 未创建 worker、
  未发出 join，也没有把 WorldFrame/entry pointer 发布到另一个线程。
- `CWorldFrameWar3+0x250` 只是 active `CBuildFrame*` publication；取消路径先清这个 borrowed
  pointer，再销毁 `CPlacementBox/CConstructUI` 相关 active resources。读者不能跨该清除边界持有它。
- stage21 的 static-root 与 `CGameState+0x2C8` 是两个不同 `CTextTagManager` 生命周期；同一
  render function 不构成跨-owner alias 证明。TerrainImage index 也只在 gate 与当前 CTerrain
  registry 都有效时同步解析。

- War3 1.27a 已证创建 EvtSched OS workers；但哪一个 Engine/main thread 触发各条 RenderQueue
  callback、是否存在未审 native async handoff，以及 upload 的固定 affinity/join 仍 Unknown。
- 三个 WorldFrame group owner 的已审 add/clear/render path 未见锁或 worker handoff，并明确不是
  lock-free publish；是否存在未审外层同步原语仍 Unknown。
- `CWorldObjects` 103 槽中哪些函数会在帧内改写 model/clip/palette 输入，以及它们与 pose writer
  的先后关系。
- reset 时 `CWorldFrameWar3`、`CDoodads` singleton 与 D3D device owner 的完整跨类调用图。
- stage18/21 的已证对象本身没有直接 skin-output 证据；UI/particle/effect 的其他未审路径是否
  持有晚于主 DIP 的同一 skin output 仍 Unknown。

在这些项闭合前，线程亲和性只能写“当前 ASM 证明同一嵌套链上的顺序”，不能写“已证明单线程”。
