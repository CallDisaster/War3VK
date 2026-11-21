# x64 小纹理上传与 EchoIsles 退出：只读调查及修复设计

日期：2026-11-17。作者请求：追查小纹理来源、上传寿命、同步等待及 EchoIsles 退出超时；暂不改代码。

本轮仅阅读当前源码、既有产物及磁盘上的 Game.dll，使用 LLVM 作静态反汇编；没有修改产品、测试、脚本或配置，没有构建、启动游戏、部署或操作共享 IDA。本文件是新增调查文档。原失败回执及 Claude 的报告均保留。

## 1. 结论与证据等级

| 问题 | 本轮结论 | 证明边界 |
|---|---|---|
| 高频小纹理是什么 | 找到原版 TerrainShadow/ListA 的 16×16 分块纹理创建、像素回调、失效标记与消费链，和采样特征高度吻合 | 静态链已确认；旧运行没有把逻辑 ID 与原生回调关联，不能认证那 32 张全部属于该链 |
| 为什么 1 KiB 更新很贵 | 每次 Unlock 做 Begin/Chunk/Commit 三次同步调用；宿主复用单一 staging，并在上传前、上传后等待 GPU/资源退休 | 当前生产代理源码和首批 P0 回执相互支持；不是传输 1 KiB 本身需要数百微秒的证明 |
| 数据由谁持有 | 前端锁定区、命令环、宿主请求、事务、GPU staging、managed CPU 副本是不同所有者；环消费不等于 GPU 完成 | 已逐段核对当前实现，见 §3 |
| EchoIsles 为什么超时 | Finish/GPU/WSI 已结算，最后局部析构的标记已写出，进程仍未及时变为 signaled；随后自然 exit 0 | 已定位到最后日志写入收尾、CRT/全局或 TLS 析构、DLL detach/OS 进程退出边界；还没有实际阻塞栈，不能指定某一 DLL 为根因 |
| 应先怎么修 | 先改宿主上传所有权和 GPU 等待，再减少事务同步；退出单独取冷路径栈再修具体所有者 | 设计，尚未实施或验证；不能承诺毫秒收益可直接相加 |

这不是“调查已完全闭合”的回执。业务来源的运行时 ID 关联、退出阻塞函数仍缺证据；下面给出补齐它们的最小范围，避免继续泛化埋点。

## 2. 小纹理来源：从统计对象追到原生生产链

### 2.1 已有实机统计

首批 P0 的 Lost Temple 两轮出现相同的 32 个高频逻辑纹理 ID：387–391、427–432、467–473、506–513、549–553、593；EchoIsles 是另一组 ID。特征为 16×16、format 21（A8R8G8B8）、单层、每张每世界帧一次整层更新，1,024 B。约 32 KiB/世界帧，却引出约 96–99 次事务同步和约 64–66 次与上传相关的 GPU 提交。

这一统计来自 [P0 报告](2026-11-17-x64-p0-sync-profile.md) §5、§10 和对应 `AutoTest/artifacts/x64-p0-sync-profile-20261117-*` 日志。不同地图不能用裸 ID 互相关联。创建一次、反复更新也不等于每帧创建/泄漏。

当前第二批源码在 `tools/render_host/d3d9_proxy.cpp` 的 `Device::CreateTexture` 中记录 `__builtin_return_address(0)`。本轮检查日期为 20261117 的既有 `game.stderr`，未找到非空 creator 归属记录；不能把源码新增字段当成已拿到运行时来源。

### 2.2 静态逆向身份与方法

磁盘原件：`E:/Work/Warcraft III/Game.dll`，13,187,048 B，SHA-256：

`E04D1716603C075EB0C8E1E21CF1093A664ADC5249EFAB396BFA08D7B09D0C3A`

所有下列地址均为这个二进制的 **RVA**，首选基址 `0x6F000000`。方法是 PE 节表解析、候选 E8/绝对引用筛选，再用 LLVM 反汇编检查实际指令与参数；不把扫描命中直接当成调用证明。LLVM 显示的 `GameMain+...` 只是最近导出标签，不是内部函数名。本文的语义名是便于讨论的描述。

| RVA | 本轮实际核对的指令/数据流 |
|---|---|
| `0x72ABD0` | 先清理旧辅助纹理；按 owner `+0x100` 的 ListA 数量建立 `+0x288/+0x28C/+0x290` 数组，每项 0x10 B；读取 0x94 B ListA entry 的 `+0x30/+0x34` 网格坐标；将辅助项写回 entry `+0x78` |
| `0x72ACD3–0x72ACF0` | 明确传入宽、高均为 `0x10`；callback=`Game+0x743E80`，context=该辅助项；out=辅助项 `+8`；调用 `0x0E37E0` |
| `0x0E37E0 → 0x0F09B0 → 0x0E6980 → 0x0E45D0` | Gx 创建入口、D3D 实现、原生纹理对象分配和初始化。原生对象 `+0x14/+0x18` 为宽高，`+0x24` 为 callback context，`+0x28` 为 callback，`+0x30` 为 D3D texture |
| `0x0F02B0` | 原版通用 D3D 纹理建立/刷新消费者；缺 COM texture 时调用 D3D9 CreateTexture，后续调用像素 callback、取得 surface、LockRect、按行复制或格式转换、UnlockRect |
| `0x0F0405 / 0x0F0440` | CreateTexture 的两个调用点，返回地址分别 `0x0F0408 / 0x0F0443`；pool=MANAGED、usage=0。它们是通用创建位置，单独记录这个 caller 不能区分业务 |
| `0x743E80 → 0x727210` | callback 只在 phase=1 时生成数据；从 context `+0xC` 找 owner，按网格坐标生成 16×16 块。32 位像素分支输出 pitch=0x40、256 个像素；另有 16 位分支 |
| `0x7272C2–0x727423` | 像素来自 owner `+0x240` 字节网格、`+0x280` 活跃窗口和 `+0x9DC`（32 位）/`+0x19DC`（16 位）颜色表；`+0x21E0` 与字节 bit 0x10 参与颜色索引选择 |
| `0x742500 → 0x7424E0 → 0x7424C0 → 0x0E38A0 → 0x0E6BC0` | 将区域转换到 16 像素分块，找到 ListA entry `+0x78` 的辅助纹理并标脏；零矩形走整层失效逻辑 |
| `0x737500` | ListA 渲染取 entry `+0x78` →辅助项 `+8` 的纹理，送入 `0x704970` 的渲染状态路径 |
| `0x7217B0` | 逐项释放辅助纹理并清引用；`0x72ABD0` 重建前调用它。不能把原生重建直接描述为永不清理 |

与已有 [ListA 上游逆向](war3_render_issues/23_blob_shadow_lista_upstream_reverse/README.md) 相互印证：这条链属于**地形阴影、边界、迷雾等混合结果层**，不是单位模型贴图，也不能仅凭纹理形状命名成“纯战争迷雾”。本轮独立看到了相同的 `+0x104` ListA、0x94 stride 和 `+0x240` 字节网格。

**缺的最后一段证据：**运行时 `(session, resource id, generation, level)` 是否确实连到 callback `0x743E80`，以及内容是否真的每帧变化。尺寸、频率和分配顺序高度吻合，但不是这个关联的替代品。未枚举所有可能的间接创建入口，不声称它是全游戏唯一 16×16 来源。

下一轮若获准实现最小取证：在资源创建/首次目标更新时，保存有界的原生创建上下文/已验证 callback RVA，与 COM 对象和逻辑资源代际一次性关联；热路径只查已有归属。可以利用上述精确链验证来源，不做每帧全栈扫描，也不解引用未经验证的猜测 Game 指针。分开记创建与更新调用点，因为 `TextureSurface::UnlockRect` 会转入 `Texture::UnlockRect`，后者的直接 caller 可能只是代理自己的包装函数。

来源确认前，**不关闭这些纹理，不跳过迷雾/阴影更新，不按“每帧一次”合并跨 draw 的更新**。如果以后测到大量内容完全不变，可另提完整字节等价去重；仅 hash 相等、相同尺寸、相同 frame 不足以授权省略。

## 3. 实际上传生命周期与复用条件

当前真实命令环路径（不是旧命名管道原型）：

`Game 像素生成 → x86 Lock backing → 行打包 → 命令环 → host Packet → TextureUpload 事务 → GPU staging → VkImage`

同时，成功事务还将数据更新到 host 的 managed CPU content，以支持以后锁定恢复/读锁。

| 数据 | 所有者及写入 | 何时可复用/释放 |
|---|---|---|
| 前端 Lock backing | `Texture::LockRect` 获取本地 level 或受预算约束的上传窗口；READONLY 与写锁分开 | 成功 Unlock 后释放窗口或解 pin；失败必须遵循现有 lock/abort 合同，不能提前当成功 |
| 前端 packed payload | `UnlockRect` 逐行拷贝到栈上有界数组 | 当前同步 `checkedCall` 已消费入传输后可复用 |
| 共享命令环 | producer 将 header/payload 复制入环 | **consumer 复制到自己的 Packet 并发布 head 后**可复用，不需等 applied，更不是等 GPU |
| host Packet | `Consumer::tryRead` 复制整个记录，校验序号/长度/checksum/nonce | 本次 apply/append 已复制走之后；不能向后异步保存栈指针 |
| TextureUpload 事务 | Begin 分配完整区域 vector；Chunk 按 token/连续 offset 追加；Commit 必须完整 | staging 与 retained CPU 副本均已取得数据后；Abort/失败按事务规则结算 |
| host GPU staging | 当前是可覆盖的单一 coherent buffer，写入前 settle、提交后再 settle | 必须等 GPU 最后一次读取退休；CPU 已解析或 applied 不能证明可覆盖 |
| managed CPU content | `Entry::content`，按完整 texture layout 保存，成功 Commit 后 patch | 受资源代际/销毁控制；不能因有 GPU image 就无条件删除，否则后续 READONLY/恢复锁会失真 |
| VkImage | DXVK 资源引用及 barrier/last-use 跟踪 | 同一图像旧 draw→更新→新 draw 必须按序；资源销毁遵守 GPU 退休 |

`d3d9_command_ring.h:269–309` 的事实尤其重要：`get(... packet ...)` 后，`head.store(...)` 在执行 apply **之前**；applied 在执行之后另行发布。二者与 submitted/completed/retired 是不同进度域。

因此，对已有 upload-ownership 草稿作两点校正：

1. 环槽位的复用门是 **head**，不是 applied。若让 GPU 直接读取命令环 payload，现有实现会允许 producer 太早覆盖，必须另设 GPU 生命周期租约；本轮不推荐这样起步。
2. 以游戏写完 Lock 数据为起点，当前环路径有 **6 次 CPU payload 拷贝**：backing→packed、packed→ring、ring→Packet、Packet→事务、事务→staging、事务→managed content（retainContent 开启时）；另有 GPU buffer→image。不要把游戏生成像素算作上述一次，也不要重复计算 producer 内不存在的额外整包副本。

事务细节见 `d3d9_texture_wire.h`、host `Session::apply` 的 Begin/Chunk/Commit/Abort 分支：资源引用/代际、revision+1、首传整层、区域范围、完整字节数都属于准入。当前只有一个 active transaction；如果后续改多事务，必须明确每事务字节额度，不能只扩大容器。

`ReadContent` 从 host retained CPU content 恢复数据，不能统一称为 GPU readback。它仍是客户端依赖返回数据的同步调用。纯写锁、读锁、真正 GPU 读回和 EVENT Query 需要分开处理。

## 4. 同步为什么会放大成本

`D3D9Backend::uploadTextureRegion` 当前顺序（约 1466 行）：

1. 校验 id、subresource、区域及字节数。
2. `owner.settle()`：刷出当前 stream/group，等待 GPU timeline，并等资源退休。
3. 必要时重建共享 staging；复制本次数据。
4. `owner.begin()`，记录 copyBufferToImage。
5. `owner.submit(..., true)`：等待 CPU 提交结果，再等待 GPU/资源退休。
6. 才发布 initialized/uploaded 标志并返回 Commit 回复。

因此纹理更新会反复切断本来连续的 draw 流。上传前的 settle 可能提交前面 draw，上传本身又单独提交；约 32 次更新即可产生约 64 次相关提交。计数上不是严格每张必有两次非空提交，具体取决于前方是否积压命令。

已有实机窗口（不是本轮新测试，也不是严格 A/B 性能认证）：

| 项目，ms/世界帧 | Lost Temple IMMEDIATE | EchoIsles IMMEDIATE | 生与死 r2 |
|---|---:|---:|---:|
| 平均世界帧时 | 47.3 | 43.2 | 148.2 |
| x86 全部同步墙钟 | 41.63 | 37.22 | 82.27 |
| 其中纹理 Unlock 三段事务 | 29.56 | 24.93 | 29.73 |
| 其中 StageMarker | 3.04 | 4.02 | 35.02 |
| 环满等待 | 0 | 0 | 27.65 |

x86 同步墙钟包含宿主执行，宿主执行又包含 GPU 等待；不能把这些毫秒相加成“可优化收益”。没有 GPU timestamp 的数字不是纯 GPU 执行耗时。

生与死新增既有 ring16 回执将环满等待降到 0，但 StageMarker 等待升到 55.3 ms/世界帧。其窗口短、CPU 背景负载不同，不认证扩环 FPS 收益；它支持“宿主消费能力仍不足，等待位置发生转移”的判断。`CasterCopy` 36.5 ms 和 `DiscardCarry` 30.2 ms 是整条路径墙钟，包含分配/改名/整个 addCaster，不是两项纯 memcpy 成本。

## 5. EchoIsles 退出时间线与尚未证明的原因

证据目录：`AutoTest/artifacts/x64-p0-sync-profile-20261117-echoisles-r1/`。

对应前端 `front.dll`：1,220,608 B，`E64AE3909250848432DC5FEF6B481CA48CCFB615CBA08B412DC2068C053CA90A`。

对应 `host.exe`：7,667,712 B，`11E50BCFBAAB7E032DF7C64062D036D75827D811E64503756B0C1158998BB2E5`。

只对齐实际游戏宿主 PID 5348；同一日志中两个短命初始化宿主 52672/16012 已正常退出，不与游戏宿主混算。

| 事件 | 现有日志 |
|---|---|
| Finish 回复已写 | `host.log.2`，tick 684396390，processElapsed 81890 ms |
| before / after session.reset | 两者均 tick 684397875；从 Finish 回复到这里 1,485 ms |
| normal-return intent / flushed | 同一 tick，code=0 |
| wmain-locals-complete 标记 | 同一 tick；MainExitWitness 是 wmain 首个局部 witness，末尾执行其析构 |
| 前端 primary wait | 5,047 ms 后 WAIT_TIMEOUT，win32=258 |
| 前端 secondary drain | 再等 4,422 ms 观察到自然 exit 0；原失败保留 |

两次 wait 的计时合计约 9.469 s；这不是精确 Finish→退出区间，前后还有日志/调度间隙。与上述进程 tick 联合看，主要延迟已落到末尾 witness 附近及之后，而不是尚未处理某个纹理 Commit。

宿主最终账本：accepted=861817；submitted=gpuCompleted=retired=74137；liveResources=0；normalTeardown=true；presentQueued=presentApiSucceeded=presentGpuCompleted=presentRetired=1353。

当前 `D3D9Backend::finish()` 也明确在成功返回前 settle、结算 WSI、核验上述恒等式、shutdown 并清 Owner。随后才允许 Finish 成功回复。这和日志互相支持：不能把这次超时称为“还有游戏渲染队列没有排空”。

两个重要限制：

- `wmain-locals-complete` 是在最后 witness 析构内调用 `ExitMilestone` 写出的。其 timestamp 在同步 `WriteFile` 前取得，看到行不严格证明该 WriteFile 已返回。因此剩余范围还包括**这一次末尾日志 I/O 的完成**，不能断言整个 wmain 已经返回。
- 现有证据没有慢时刻的线程栈/CPU 调度/I/O 完成记录。CRT 全局或线程局部析构、DLL detach、驱动清理、磁盘等待、低优先级调度均只能列为候选。不能单凭 NVIDIA 模块存在就归责驱动。

同类现象不只发生在 Echo：生与死 r2 的 Finish→末尾 witness 仅 375 ms，primary 5,000 ms 超时，secondary 1,812 ms 后自然 exit 0；Lost Temple r1 primary 688 ms 正常。地图/负载会影响表现，但还不能证明地图逻辑是原因。

`stage=setup` 来自 `OwnedChild::wait` 使用通用 `Stage::Setup` 分类，不表示这里仍在启动。后续应增加/使用精确的 exit-wait 阶段标签，避免再次误读。

## 6. 具体修复方案与顺序

### P0：最小补证，分别关闭两个归因缺口

1. 小纹理：只对目标尺寸/格式的首建或首更新关联源端 callback/context、逻辑 ID/代际和阶段；有固定容量、溢出计数，unknown 明确保留。还应分开计首传/重复更新、实际改变字节、创建/锁定/上传 API 路径。不能只再收一个通用 CreateTexture 返回地址。
2. 退出：在隔离测试中保留 exact host handle、PID+creation identity；在其超过正常退出窗口时采集受限的宿主线程栈和调度/I/O 证据，覆盖最后 witness WriteFile 返回、CRT/全局析构、DLL detach 到 process-signaled。优先独立观察，不在 loader lock 中加入堆分配、锁或复杂日志。保持旧红回执和原始 primary timeout，不以 secondary exit 0 升级通过。
3. 先取得一轮没有并发构建/实机争用的对照，记录双进程 CPU 时间和优先级。现有日志阶段 1.485 s 也应单列，不能默认为正常零成本。

以上是下一轮实施/取证范围，本轮没有安装探针或运行这些实验。

### P1：先解开宿主 GPU 上传寿命，保留现有线上事务

修改范围应限于 `dxvk_core/d3d9_backend.*` 的上传 owner/stream 集成及相关行为测试：

- 用 **DXVK staging slice** 保存每次上传数据，绑定 command list 与 GPU retirement；消除“单一 staging 必须清空 GPU 才能覆盖”的设计约束。
- 将上传记录进和 draw 保序的 context/stream，保留 DXVK 图像屏障与使用追踪。旧 draw→上传→新 draw 不得重排；同帧多次写同一纹理也必须按顺序可见。
- 不在每次上传强制 `settle/submit(true)`；在合适的提交边界/额度压力处 flush，只有读回、必要 fence 或 staging credit 不足才等待。
- 设置在途 staging **字节额度与最大块数**，按完成 fence 回收；不足时有界背压，不能用无限分配换 FPS。额度需要实测，不在本设计里擅自提高产品内存预算。
- 稳定的提交状态存储也必须按在途寿命拥有：当前单个 submission status 的 CPU 等待保护不能直接删除后继续复用原地址。
- 分离“上传已记录/后续 draw 可按序使用”与“GPU 已完成”。在成功记录后才发布逻辑初始化状态，异常继续 fail-closed；资源销毁、Reset、Finish、Query 和 CPU 读回不能误用该标志替代完成证明。
- 首批仍保留 Begin/Chunk/Commit 与 managed CPU 副本，先用同一事务语义验证 GPU 等待能否降低，避免一次改协议、内存寿命和 HRESULT 三个边界。

复用依据：进程内 `D3D9DeviceEx::UpdateTextureFromBuffer` 已走 `AllocStagingBuffer` 和 `copyBufferToImage`；`DxvkContext::copyBufferToImageHw` 有资源依赖与 Init/Exec 选择，遇到已跟踪图像会保序进入 Exec。不是要求 CPU 等 GPU 才能满足纹理顺序。

注意 `DxvkStagingBuffer::alloc` 本身没有全局容量上限；进程内路径还由 `ThrottleAllocation` 与 staging fence 施加在途压力。只搬 allocator、不搬有界信用和 retirement 会重新制造内存膨胀。

### P2：再减少前端同步与小包固定成本

- 在 P1 证明正确后，对纯写上传制定 ordered one-way 发布；保留资源代际、事务 token、revision、完整区域与首传规则。
- 先区分“已发布但未 apply”“apply 失败”“终态不可继续”。将 Begin/Chunk 简单改成无回复会改变内存分配失败/HRESULT 的可见时点，必须先设计 admission credit 或明确受限的异步错误合同，不能悄悄返回成功。
- 对已证明适用的小区域可设计一条有界原子上传记录，避免 1 KiB 仍跑三个往返；保留大纹理分块路线。该项涉及协议与双方解析，不是调一个开关。
- 降低宿主事务 vector 每次 reset 释放容量的开销可独立做，但须保留总容量上限；不会把它宣称为当前几十毫秒的主因。
- 不让 GPU 直接借用已归还的命令环区域；若未来做专用共享上传窗口，必须另有 GPU 完成后的租约归还与崩溃清理合同。

### P3：退出修复按实际栈选择，性能线不掩盖它

- 如果落在末尾日志 I/O，缩小退出输出、把必要最终账本明确置于退出协议中结算，并量化开销；不能丢账或把记录失败当干净退出。
- 如果落在产品全局/TLS 析构，将具体资源转为显式 owner，在 Finish 的可观测清理阶段释放；验证引用和线程真正结束，正常 CRT 仍照常执行。
- 如果落在第三方 DLL detach，先确认精确模块/版本/等待关系，再设计加载器所有权、线程收口或卸载时机；没有栈前不试改驱动、系统或运行库。
- 保留 bounded wait 和自然退出验收。单纯延长 5 s、`_Exit`、TerminateProcess 或看到 exit-intent 就判通过都不是修复。现有故障自终止是另一条 fail-stop 路线，不能用来认证正常退出。

### P4：生与死另行细分宿主高成本路径

纹理修复不会自动消除整条 addCaster 和 DISCARD storage rename 的工作。将已有 CasterCopy/DiscardCarry 墙钟继续拆到分配、保留字节、转换、命令记录等实际责任范围，再决定复用与按范围拷贝；不能因为字段名字有 Copy 就直接上 SIMD/memcpy 优化。环满和 StageMarker 只作为背压结果观察。

## 7. 后续验收矩阵（全部是待做项）

| 边界 | 必测内容 |
|---|---|
| 上传顺序 | 同一 command list 与跨 list 的 draw-old → partial/full update → draw-new；同帧多次更新；mip/非零 level；按像素比较 |
| CPU 数据 | 首次局部写的零初始化、READONLY、LRU eviction 后恢复、未改区域保留；managed 副本与 GPU 版本一致 |
| 生命周期 | staging wrap/credit 耗尽、GPU 延迟、资源销毁重建/代际复用、Reset/换图/Finish；禁止借已回收 ring/staging |
| 故障事务 | Begin OOM、缺 chunk、重复/乱序 chunk、Commit 不完整、Abort、宿主崩溃/断链；不能半发布或静默成功 |
| 正常退出 | Echo/LT/生与死，多个重复，primary 自然退出码0；最终 CPU/GPU/WSI 账本闭合，无 owned job 遗留；secondary drain 不晋升 |
| 性能 | 固定地图/相机/配置/身份的交错 A/B；热身后帧 median/p95/p99、双进程 CPU、提交数、等待次数/时长、staging 峰值和增长；计时开/关分别记录 |

像素、VVL、自然退出和资源上限应与性能一起过门。先前“中位≤本地1.5倍/p99≤2倍”的阈值曾标待作者确认，本轮没有把它擅自升级为已批准发布门槛；输出实测比值及是否满足建议值即可。

## 8. 源码快照与本轮未完成项

调查分支 `codex/v1.22-main-tree-20260920`，HEAD `a7e1cbabae2ff6b378f9c9b1532240c6a7b814ee`；工作树存在其它任务持续写入，不能把当前源码整体等同于首批 P0 二进制。以下为本轮读取时文件 SHA-256，便于复核上述机制：

| 文件 | SHA-256 |
|---|---|
| `tools/render_host/d3d9_proxy.cpp` | `2BE8537C688A154A0C59C5F60C6AB101F08ED26A0110FF0C164D166D8AEB401F` |
| `tools/render_host/d3d9_connection.h` | `81646BB16B45B31E7764DC52DA3FAF16E8AC661AF5E9190FE219DBE76B9AEAA3` |
| `tools/render_host/d3d9_command_ring.h` | `07ED4309CC342E06BBD9FE6AA6858782085D4F912294E0E698C63E6C7FCEA802` |
| `tools/render_host/dxvk_core/d3d9_backend.cpp` | `6CB19BD589495A21C527D927A99FAFBF2D74A256DBD96443C95C49EE8810EAC3` |
| `tools/render_host/dxvk_core/d3d9_host_main.cpp` | `E56EC285DD38714AD350C14FB1B4CFB2CA76ED7190DAED9468A182D677963951` |

本轮没有新增 runtime 证据、重新评图或跑任何门禁。已完成的是生产代码寿命/等待审计、原生纹理链的静态逆向、既有退出时间线复核和分阶段设计；仍未完成的是实际逻辑 ID→原生回调归属、慢退出当时的阻塞栈，以及所有修复后的功能/性能验收。
