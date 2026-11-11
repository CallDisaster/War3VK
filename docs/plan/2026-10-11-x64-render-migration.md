# 64 位渲染迁移：重新启动实施

2026-10-11 用户明确要求：收拢当前工作回到长期主线，并立即开发 64 位渲染进程，最终迁走渲染负载。
这项新决定覆盖旧路线图的“64 位继续延期”。它不把原有 CPU 实验、天气候选或共享核心方案自动晋升为产品。
本轮不推送、发布，不覆盖玩家 DLL/地图，不修改系统或另一工作树。

## 目标与不可能省掉的边界

目标是 x86 游戏只保留必要的 Game.dll/JASS 接入、D3D9 对象代理、受限上传窗口与窗口消息泵；
x64 宿主持有渲染资源、几何缓存、管线、GPU 提交和最终显示。
原版游戏自身的模型、逻辑与脚本内存不会因渲染外置自动消失；D3D9 Lock/Readback、资源创建结果、
设备 Reset 和退出也不能简单改为不可靠异步消息。不能把“扩大池子”或“只迁诊断历史”当成实现目标。

两条路线分开：完整 D3D9 语义代理最有希望覆盖现有整幅画面；原生场景/共享核心接管仍缺完整输入，
不能凭第一条材质导出链跳过主画面、UI、特效、纹理、动画。共享核心只从唯一上游使用，不复制 FORGE 代码。

## 当前执行顺序：2026-10-29 作者转交评审后的调整

StateBlock 批次已收口：r5/r6 GPU 和 r1 生命周期回执保持，旧红门不追认。
按作者本次指示，暂停 X3 的 RT/DS、StretchRect、Reset 与更多拓扑扩展，先执行 T1/T2。
这条新顺序覆盖下面历史 checkpoint 的“完整接入后才测内存”及“下一块 RT/DS”。

1. **T1**：现有进程内 WarVK 真图固定初始相机的 VA 区域/映射文件、空闲碎片与进程账户；
   私有堆必须有分配器地址见证才能归属。预算和 GPU 字节不充当 x86 VA；诊断配置分开。
   该测量估计未来收益边界，不是已实现的 OFF/ON 迁移收益。
2. **T2**：先测现有 flight 复用的 CPU 提交/GPU wait 调用/退休分段，并补真实 API 负载分布。
   再在开发代理/宿主试共享环、状态常量差量和原 DXVK 常量分配；像素、故障恢复、VVL 不能放宽。
   已有 10-28 后续同轮 1,025 draw 约 3.6–3.8 ms 对本地 0.72–0.85 ms，约 25 ms 为旧基线；
   不能用旧数推定当前主要瓶颈。每个阶段区分实测、推断与尚缺数据。
3. **T3**：先原版画面，增强层先写 Game/设备依赖及传输清单，由作者选择范围；不直接迁 28 万行。
4. **T4**：只评估 LAA 未验证风险及现有大缓冲按需/缩小替代方案，不修改游戏 PE 或预算。
5. **T5**：随相关模块收敛，新增行为测试；本批不拆巨型生产入口、不全库格式化。

T0 临时 index 的本地 backup 提交，以及中位数 1.5×/p99 2×/迁移范围决定门仍待作者确认；
未建立提交、未改 HEAD/index。Game.dll guard、隔离桌面、玩家 DLL 保留、不发布仍保持。

首批实际结果见[T1/T2测量](../research/2026-10-29-x64-review-t1-t2-baseline.md)：
本地游戏OFF/CPU录制ON/OFF正常退出；private633–653/732 MiB，私有allocator归属
仍缺，净迁移量unknown。当前玩家/沙箱均已LAA。1,025-draw合并模式实测8次flight
复用，约占耗时51–55%；未计时远端约14.55–14.61ms、本地1.74–1.76ms，尚未过性能门。
真实census已复跑，但缺逐帧、Lock/Unlock/GetData字节与频率。**下一实现优先补这两份
测量缺口，再做共享环/差量/原生常量分配原型；T1/T2尚未完成，不恢复X3扩展。**

2026-10-29 Claude 接手后先做了不依赖游戏运行的宿主侧原型：默认关闭的
`WARVK_D3D9_STREAM=1` 流式命令列表 + 原生常量环，见
[流式回执](../research/2026-10-29-x64-stream-constant-ring.md)。所有像素/VVL/故障/寿命门
通过，FF draw 在宿主侧不再排空 GPU。安静轮 1,025 draw 较 group 低约 24%，仍为本地
3.4–4.7 倍；剩余成本在 EndScene 前的准入/传输段。T2 后续顺序调整为：
① 拆解准入段成本 → ② FF draw 异步批次 → ③ Unlock 流内上传/DISCARD 换存储 →
④ 共享内存命令环；T1 allocator 区间与真实逐帧 census 仍需游戏运行，与之并行。

① 已完成，见[准入段拆解](../research/2026-10-29-x64-admission-breakdown.md)：每批往返
完全串行（69 µs/批，宿主 apply 65%、管道+唤醒约 29%）；宿主 apply 中 DXVK 逐 draw 无条件
重设状态+提交约 1.0 µs/draw，8 槽采样器循环可忽略。据此在 ② 与 ③ 之间插入两项：
②a 宿主录制按原生方式做脏跟踪；②b 预检后先回复再录制，让传输与录制重叠（④ 的前置）。

② 已完成（默认关闭），见[固定功能批次](../research/2026-10-29-x64-fixed-function-batch.md)：1,025 固定功能 draw
从同步 45–47 ms 降到 7.8–9.2 ms，仍为本地 2.3–2.8 倍。批次剩余成本以每条 3,180 B 完整状态的传输/解码
和 FF 常量每条重算为主，因此在 ②a 之前先插入：②c 固定批包内差量编码；②d FF 常量按输入变化重建。
setter（采样器/效果/阶段）仍是有变化才发的同步调用，待真实 census 计数后再定。

**2026-10-30 调整：内存优先。** 作者明确：迁移的初衷是减轻 32 位进程内存压力，内存优先于继续做
②a–④ 的性能。开发代理此前为每个 VB/IB/纹理在 x86 留完整副本，接游戏后会抵掉移出 DXVK/驱动的收益。
本批新增默认关闭的 `WARVK_D3D9_X86_BACKING_KIB`：宿主保留 VB/IB/纹理的 x64 权威副本，x86 按 LRU
预算只留未钉住的副本，Lock 或索引 draw 碰到已淘汰项时用 `ReadContent` 读回，见
[内容驻留](../research/2026-10-30-x64-x86-content-residency.md)。合成 64 MiB 纹理工作量下 x86 已用 VA：
进程内 DXVK 739.9 MiB → 代理预算 0 为 59.5 MiB，像素一致；其中约 617 MiB 是进程内 Vulkan/DXVK/驱动基线。
内存方向后续：DISCARD 换存储与有界上传窗口（即原 ③）→ 定默认预算 → 游戏接线后同图 x86 VA A/B；
性能项 ②a–④ 排在其后。

## 已完成落点：2026-10-29，固定功能执行开始闭合

用户要求先把 Warcraft 完整接入，再量 Storm 与 32 位客户端的迁移开／关内存。
本批未做新的内存测量，Game.dll 保护仍在。

2026-10-29 后续：Alpha Test / table、vertex、range fog / scissor / separate alpha blend
的 76 B 状态交接已通过真实默认和强制优化 GPU 对照；四端各 269 张完整图完全一致，
60 个实际非法请求及恢复通过。补上 Clear 的 scissor 规则并保留测试红门，
96 轮资源/Query 生命周期亦通过。随后八纹理 stage/独立 sampler 的默认和优化 GPU 门
`x64-multitexture-gpu-20261029-r3/r4` 通过，四端各 286 完整图相等、72 个实际拒绝/
恢复通过；C68506CF 宿主 96 轮资源生命周期/故障恢复亦通过。
随后 StateBlock 的选择性录制/恢复、资源私有保活和 canonical null texture 的
programmable draw 已接通；80287B63 宿主的 `x64-stateblock-gpu-20261029-r5/r6`
强制优化/默认门均通过，四端各 297 图完全相同，72 个实际拒绝/恢复通过。
新增 null VB 偏移保留与较短 VB 的 Apply 前拒绝；失败轮与原因均保留，详见
[StateBlock 记录](../research/2026-10-29-x64-stateblock-integration.md)。新候选的
`x64-stateblock-lifetime-20261029-r1` 96 轮/576 资源/256 Query 与 raw 故障恢复亦通过，
最终资源归零、宿主正常退出、source/reference/player 保持。
下一块是真实 RT/DS、目标切换和分辨率/Reset。更多拓扑、caps、WarVK pass 和实际游戏图像仍未闭合；不得将开发门
计为 Warcraft 完整接入，内存测量继续留在完整接入之后。

- 原 Factory 规则已接到 x64，创建设备前可协商；局部 viewport/矩形 Clear 走原 DXVK。
- 共用生产 FVF、FF key 和变换常量准备，补紧凑顶点输入、世界/纹理变换、材质、
  方向光、POSITIONT，以及 FF/SM3 混合执行。首批为单纹理，后续扩到八纹理有限 profile。
- 持续绘制暴露并修复了原快链/优化 pipeline 专门化常量不同步；原实现打包段
  由两端共享，双位宽各 200,000 差分检查通过，实际有效配置写入验收。
- BGRA8 的 1024 尺寸内多 mip、NPOT、分块/局部上传和 LOD 已接通；默认与强制
  优化管线两轮隔离通过，各四个渲染场景每个 31 张图 / 507,904 通道完全一致。
  30 个实际故障事务及后续恢复、VVL365 零 warning/error、原 GPU/use 退休与退出通过。
  x86/x64 wire CPU 各 1,150,226 检查。仍不是实际 Warcraft 画面。
- FF 目前保留同步有序事务，不把既有 SM3 grouped 配置称作 FF 性能优化已完成。
  本地共享代码 fresh ED72F52E DLL 与本次配置 119/119 回归通过；不得继承旧配置总数。
- 后续 16 格式/表面与 sampler 已独立通过；最新 E7EFBD5E 宿主在默认/强制优化下
  四端各 225 图 / 3,686,400 通道实际哈希相同，48 个非法事务及恢复通过。
  共享 sampler key 的 x86 26B411E9 DLL 实际 119/119；96 轮资源寿命通过。
  Alpha/fog/scissor 候选仍在下一独立验证批，未计入上述结果。

接下来按真实纹理格式/表面接口、更多 sampler/stage、RT/DS/StateBlock、
Reset/换图及 WarVK 增强 pass 的顺序补完整覆盖。原 SetMaterial Ambient 与太阳改写
已单列迁移边界；本轮基础 FF 用例没有假装覆盖它们。
确切候选、红门与通过证据见
[固定功能接线回执](../research/2026-10-29-x64-fixed-function-integration.md)。
只有实际游戏同场景完整渲染后才进行 Storm/客户端/x64/GPU 内存账。

## 前序落点：2026-10-28，入口已恢复，x64 draw 的串行等待开始收敛

- 2026-10-24 的低地址模型域、按需 TLS backing、NetEvent 初始化和管道分片修复，
  已通过一轮隔离 Lost Temple 的载图、实际截图、退图及自然退出。此前 10-23 的失败
  回执继续保留；最新入口证据见 [低地址修复](../research/2026-10-24-mdldata-low-address-repair.md)。
  这部分减量来自本地 x86 修复，不能计入 x64 迁移收益。
- 本轮明确把“原版 D3D9 API → 类型化有序消息 → x64 原 DXSO/DXVK”作为主线。
  实际受限 COM 绘制已在 x64 宿主执行，x86 测试调用者不加载 Vulkan；尚未接管游戏整幅画面。
- 新增最多 32 draw 的按值批次，同一批全部检查通过才记录 GPU 命令，每个 draw 使用
  独立对齐常量切片。资源更新/销毁/Query/Readback/Present 保留有序边界；旧 wire
  布局不变，新 opcode 由旧读方拒绝，开发批处理默认关闭。
- 宿主的提交与资源退休改用原 DXVK 队列通知，同时修复生产 Win32 条件变量的
  deadline 符号、虚假唤醒和毫秒转换问题。32/64 位实际 CPU 各 1,014 检查通过。
- 在相同 97-draw 离屏小场景，初始逐 draw 约 170 ms，批处理约 8.7 ms；去除固定
  Sleep 轮询后，批处理约 0.76–0.88 ms，本地参考约 0.13–0.17 ms。逐 draw 常量/位置
  变化按独立像素判据检查，本地与远端采样一致；29 组错误批次与后续恢复通过，VVL365
  独立回调无 warning/error。数字来自隔离微基准，**不是游戏 FPS 或整机迁移收益**。
- 共享修改的新 x86 DLL 已完成 127/127 配置回归及一次同 35 秒限制内的实际载图、
  截图、EndGame 和自然退出；此前两次启动/总时限失败保留，间歇耗时原因未证实。
- 四批在途和异步批次已完成 97 / 1,025 draw 各 16 场景及三场景 VVL365。
  每个在途常量槽只有在原 CPU submit、GPU timeline 与 DXVK use tracking 均满足后才能复用；
  更新、销毁、六类 ID 换代和合法恢复均经过真实 GPU 像素检查。
  高 draw 数同轮中位数由逐批等待 42.65–44.36 ms 降至 24.64–26.64 ms，
  但本地参考为 2.06–3.31 ms，**远端额外成本仍不可接受，不是性能门通过**。
- 97 draw 场景的 44 次 EVENT Issue 引发 2,048 / 2,738 次 Poll IPC。
  下一修订已实现一次性 GPU 时间线完成通知：x86 GetData 本地观察、有界单个在途等待、
  x64 原时间线完成后才发布水位。新宿主 FE6ECB 的 97 / 1,025 draw 各 8 场景、
  三场景 VVL365、五种真实管道故障、x86/x64 各 210,589 + 1,319 CPU 检查通过。
  重复 Issue / 多 Query / 销毁复用与错误回复均覆盖，通知不授权 GPU 资源释放。
  97 draw 同轮的 6,261 / 5,723 次 Poll 降为各 46 次 WaitEvent；完成时间仍慢于本地，
  高 draw p99 未全面改善。测试后的只读采样有另一游戏进程负载，性能不晋升为接受。
  详见 [本轮证据与成本](../research/2026-10-28-x64-draw-batching-and-cost.md)。

2026-10-28 后续批次已新增四消息合并 GPU 提交、定宽 codec 和真实非索引 triangle-list，
并采父/宿主 CPU 时间、周期计数与系统 busy。最终 1,025 draw 的提交合并约减少 7–13% 完成
中位耗时、30% 双进程 CPU 周期；97 draw 约减少 20–24% 中位耗时，仍慢于本地。批内重复状态省略虽通过像素检查，
性能收益不稳定，保持默认关闭。新增测试还纠正了小 viewport 下误认整面 Clear 的判据；
子视口/矩形 Clear 本身尚未实现。确切范围、失败回执与版本见
[提交合并与非索引覆盖](../research/2026-10-28-x64-submit-groups-and-nonindexed.md)。

下一性能门优先减少 DrawBatch IPC 事务和重复状态/常量载荷：GPU 提交已合并，但 1,025 draw
仍有 33 个消息。须保持固定有界窗口、整批原子预检、按值身份与原 GPU 退休，不能通过
放宽完成条件换数字。CPU 周期不按固定频率换算毫秒；短窗口 CPU 时间为零不等于零成本。
继续做同场景完整延迟/CPU/背压与长时尾延迟对照，尚未证明无竞争负载或游戏性能接受。

后续功能顺序保持：原 Factory 能力协商 → 原 FF 完整状态/着色器执行 → 纹理多子资源与
真实 RT/DS/StateBlock → 实际游戏 x86/x64 同场景等价与帧时间/内存 A/B。真实 census
显示大量 SetFVF/SetTransform/SetMaterial/SetLight/TextureStageState，另有 StateBlock、
DrawPrimitive、StretchRect 与多 mip 纹理；本轮仅补齐 DrawPrimitive 的非索引 triangle-list，
当前 SM3 小型 profile 仍不能冒充完整覆盖这些路径，子视口 Clear 等价也待补。
Game.dll 保护仍保留；玩家 DLL 不变，未提交、部署或发布。

## 历史落点：2026-10-23 模型高地址分配已取得真实关联证据

- 默认关的MDLDATA诊断已实现并接到精确Game/Storm版本；不是新的allocator。
  Core/实际ABI/生产二进制往返与独立DLL构建已通过。本批没有恢复强制设备锁或修改分配策略。
- 同候选ON r2真实记录19次分配，其中9次返回超过2GiB；观察到高返回→bucket/global入链→
  低node unlink前相同backlink。合法取反哨兵与真正返回地址分开，不再只有dump数值推导。
- 独立OFF再次自然异常于Game+12D96D，低arena/model source保留；ON r2的unlink虽返回，
  仍无实际store地址与邻居写后状态证明，不能将返回解释成结构正确。两进程的证据不拼成一条lifetime。
- **真实游戏入口仍红**：三轮均未成功连接控制管道，未取得ready/截图，ON两轮超时、OFF自然AV；全部隔离收口。
  不是地图覆盖、正常退出、完整x64渲染或游戏内存收益，代理Game.dll保护继续保留。
- 下一实施顺序：①复用唯一DBWIN监听器收齐现有初始化阶段消息，或单独默认关有界stage见证；
  ②补实际consumer/邻居结果与原生分配路由所缺边界，设计此模型域的低地址兼容方案，
  保留低地址、合法哨兵及原有zero/free/realloc/失败语义；③生产正反例和隔离游戏A/B；
  ④恢复原Factory/FF完整状态、多子资源纹理及整幅画面接入主线。不得泛改jg/not或扩大Arena求通过。

精确候选、测试数、三轮结果和独立审查见
[MDLDATA分配探针checkpoint](../research/2026-10-23-mdldata-allocation-probe.md)。
本节更新下面历史条目的“高返回未采到”状态，不追认其旧产物或将游戏红门删去。

## 前置落点：2026-10-23 有界 census 修订与模型链分配追踪

- 诊断不再强制设备多线程锁：局部参数/局部FF key → 单次try-cell → 成组API/事实事务 →
  锁域内按值copy/序号 → cell外生产serializer。未知设备状态保留missing，拒绝观察非同刻、
  与accepted calls不可直接相加。默认编译门仍关闭；没有改变游戏/分配器/显存预算。
- 两次定向Python各134项、两次实际x86隔离CPU各569检查/3条生产往返通过；固定wrapper6104B、
  copy6072B，测试窗口0堆分配；x86生成代码hot路径无重试自旋，冷ordinal独立说明。
  fresh构建r1源漂移/no-work失败保留，r2重捕获输入、再生成/build/no-work及保全通过。
- **游戏入口仍未验收**：同候选两份全新隔离实例均120秒无ready/截图而Job强制清理；
  各有2/4条严格v2日志（forcedDeviceLock=false、BehaviorFlags66），不等于地图覆盖或正常退出。
  不宣称移除诊断锁修好了Game崩溃，不将两次超时归成同一0xC0000005。
- 高地址来源追到ModelCreate的MDLDATAHash分配请求22404B→Storm ordinal401、
  Game+0x1292D0的backlink实际写指令；保留低节点名UBirth。当前产品尺寸路由原生透传，
  TLSF大块低2GiB保护不覆盖；不能凭尺寸认证未保留高节点的Storm内部arena/VA分支。
  四字段数值重建吻合，但高节点实际返回/存活/首次写史缺失，未补丁Game或改LAA。
- 下一项收窄为该模型域有界alloc/free/realloc寿命与入链/unlink关联证据，再决定低地址域/
  容器表示修复；须保留正常低地址及合法哨兵行为。真实游戏入口/正常退出仍是准入门，
  之后才继续Factory、FF完整状态、真实多子资源纹理及x64整幅画面验收。

候选、逐轮失败/通过、独立审查及证据边界见
[try-cell checkpoint](../research/2026-10-23-census-try-cell-checkpoint.md)和
[高地址分配来源](../research/2026-10-23-high-address-link-allocation-followup.md)。
仍无完整Game.dll远端画面、游戏内存节省或发布结论。

## 前置落点：2026-10-23 真实游戏入口调查与共用 FF

- 按作者新要求推进实际游戏接入：复用原 DXSO/DXVK，不重写第二个渲染器。
  共用固定功能编译器已从 D3D9Device 依赖中抽出；同一游戏进程采到的 3 VS+1 FS 配置，
  x86/x64 的 4 份原始 SPIR-V 字节完全一致。其地图阶段未证明，不是远端游戏绘制验收。
- 编译期默认关闭的有界 D3D9 census、严格读方、双位宽 key 编译工具与隔离调查 runner 已接线。
  fresh x86诊断/x64工具、census-off x86全量构建/no-work及113项定向Python门通过；
  生产census CPU422检查/0失败；均不替代全库回归或游戏门。
- **实际游戏入口红门保留**：诊断r1超时，r2发生真实Game.dll写访问违规；
  同配方稳定/本批前LLVM控制可取得ready+内部截图，而当前census-off两轮仍无ready/截图。
  后两者是超时，不擅自归类为同一崩溃。全部失败、精确DLL/原始证据和强制清理记录保留。
- 已找到并离线解析r2真实dump，纠正runner漏收WarVK/Crash；当前线索为Game链字段的
  符号/取反解码，分配来源和最初错误仍未证实。源EXE的LAA真实为true，未修改。
  不把高地址嫌疑当根因，不批改Game跳转，不把census关闭后仍失败归为“强制锁已定位”。
- 下一批先闭合失败输入/分配来源及无全设备锁扰动的有界采样，再做Factory能力协商、
  FF状态/常量和真实纹理子资源传输。原idle fault/产品owner/退出/Reset门继续开放。
  **尚无完整Game.dll画面交给x64、游戏内存节省或发布结论**，开发代理仍拒绝Game.dll。

本批具体实现、对照、故障证据与按层工作包见
[真实游戏输入与共用固定功能编译器](../research/2026-10-23-native-game-census-and-shared-ff.md)。

## 前置落点：2026-10-23 X1b 与宿主窗口 GPU 边界（后文保留历史阶段）

- Connection 已从三处 Sleep 轮询改为固定事件通知，队列/done 仍是唯一进展依据；
  只有 Clear 异步接纳，完整产品异步 owner/非阻塞退出尚未完成。
- 相同正式源、固定 x64 host/VVL365，两次独立 x86 构建执行完整六例离屏 GPU 子集均通过；
  96 轮 COM 的 2244 命令、256 Query 和退出分别 3719/1844ms，未放宽原 45 秒门。
  真实 IPC 故障 5/5 与双架构 CPU 各 463 断言通过。新测试不能抹除旧超时或替代完整 Presenter 门。
- 同一 host-owned HWND 适配器完成真实 Win32+IPC 八场景 CPU 门，并接入正式开发宿主。
  新独立 x64 build/no-work、静态21/21+73变异及GPU读方22/22通过；两轮各自新x86构建/非输入Desktop，
  top-level/child × 八帧绘制/256Present/静止GPU父窗销毁各6/6通过，VVL365零warning/error。
  primary exit均93–265ms，原5秒门不变。child外部提前销毁单列，不能写成正常保活。
- **当前下一项**是无后续命令时的窗口fault/host退出通知和owner取消排空闭环；
  复用原quiescent清理判据，不伪造Present或CPU确认。之后是真实surface重建、GPU在途故障与完整C2组合回归。
  已锁存fault通知不自动覆盖UI消息泵完全卡死；产品非阻塞owner仍未完成。
- X3 完整 D3D9/WarVK pass、X4 游戏画面与 x86 VA/commit/尾延迟尚未完成；
  不允许游戏加载当前开发代理。没有新增玩家内存节省/FPS/发布结论。

本轮身份、成本、两轮回执及测试缺陷保留见
[X1b 事件唤醒与完整六例开发门](../research/2026-10-23-x64-notification-waits.md)；
最新窗口/Presenter接线、两轮GPU回执及下一批边界见
[宿主窗口寿命 checkpoint](../research/2026-10-23-x64-host-window-lifetime.md)。

## 本批责任链

1. 当前候选收口：天气功能已有预览包；前台艺术/性能、真实天气换图/Reset/resize、自然退出保持待验收。
   不继续扩充天气功能、不假报发布完成，也不让这些人工门无限阻塞独立 x64 实施。
2. 长期主线窄修：检查作者对象 Reset 在内存紧张时的异常/所有权，优先消除 noexcept 路径的新分配。
3. RH2：独立 x86 客户端与 x64 Vulkan 宿主。宿主创建真实几何/管线并 DrawIndexed 到外部图像；
   同适配器、同驱动、导出/导入能力实测通过后，x86 导入并通过双向 GPU semaphore 交还资源。
   仅测试读回检查像素；绝不把逐帧 CPU 图片来回搬运作为产品方案。
4. RH3：独立 x86 HWND/消息泵，x64 创建 surface/swapchain 并 Present；验证 resize/recreate/close。
   这是未来让 x86 不必保留 Vulkan device 的前置，而不是全游戏迁移成功。

RH2/RH3 与 RH0/RH1 使用不同 magic/capability/管道名，不通过修改 Echo 接受资源。
继续复用精确 OwnedChild/Job、logon SID ACL、实际对端 PID 与 creation time、有限整包截止和最终 I/O 结算。
现有阻塞 lab owner 只能在外层 watchdog 管理的独立进程运行，不能直接链接渲染线程。
GPU 资源释放分别依赖 GPU 完成与进程结算，CPU ACK 不是 GPU 最后使用证据。

## 后续实施顺序和验收门

| 阶段 | 产物 | 不能冒充 |
|---|---|---|
| X0 本批 | GPU 跨位数/WSI 硬前置、Reset 窄修、明确的迁移边界 | 游戏负载已移出、游戏内存已经下降 |
| X1 | 异步产品 transport owner、有限 upload ring、严格资源 ID/generation/epoch、故障退出 | 实验室同步 RPC 可以上热路径 |
| X2 | 一条真实 D3D9 创建/更新/绘制/销毁/Present 路径交给 x64；输入与原路径对照 | 只迁阴影等于完整渲染迁移 |
| X3 | 纹理、VB/IB、shader/stateblock、query/readback、swapchain/Reset/多pass 的兼容覆盖 | 单场景正常等于所有地图正常 |
| X4 | 组合游戏画面/压力/换图/退出和 x86 VA/commit、尾延迟对照；逐项退役本地所有者 | 同时维持两套大缓存也算减内存 |

## 2026-10-11 实施进度（不等于迁移完成）

- X0：Reset 无分配窄修两宽度测试和生产 TU 编译通过；RH2 跨位数 GPU、RH3 隐藏 HWND 的 x64 Present，
  以及实际 DXVK x64 core 都已有独立实跑。validation layer 仍因本机加载条件未通过，不消除该门。
- X1：固定4槽命令队列、WRC1资源身份/私有提交、x86 I/O线程→实际x64 DXVK缓冲资源已经接通。
  两代资源、分块/局部上传、销毁及旧代/错地图/越界反例已有四场景实际证明。
  **产品 transport owner、游戏入口多线程序列化和完整故障退出仍未实现**，不能把实验 I/O 线程当产品服务。
- X2 已完成**受限离屏切片**：真正的 x86 D3D9 COM → x64 现有 DXSO/DXVK，纹理/深度/混合、
  分块/局部上传、坏 shader 输入拒绝后恢复、6500ms idle 后继续，以及 query/readback/正常资源退休已有实际证明。
  原 DXVK Presenter 尚未接到代理，`Present=E_NOTIMPL`；不能将 X2 整阶段标完成。
- 新发现并保留红门：旧单进程参考图像与远端一致，但其 PostFX/ImGui 内部 public COM 持有导致最后剩 15 refs。
  下一小批先闭合这条内部所有权/退出问题，然后接 Presenter；参考生命周期失败不被渲染通过覆盖。
- X3/X4：仍未完成。没有游戏渲染迁出量或内存收益测量；已有天气预览/玩家 DLL 未被换成远程渲染器。
- 紧接着的实施链详见[真正的 D3D9 边界](2026-10-11-x64-d3d9-migration-boundary.md)：
  D0/D1 局部共享与受限代理已有实现；D2 的离屏 draw 已执行，剩 Presenter 与上述参考退出红门，随后扩大兼容矩阵。
  本切片最终证据见[第一条真实 D3D9 绘制回执](../research/2026-10-11-x64-d3d9-first-draw.md)。

实际回执与未通过项统一见[本批 checkpoint](../research/2026-10-11-x64-render-migration-checkpoint.md)，
后者保留各版本源码/产物身份。不要把若干原型的百分比平均成“全部渲染已迁移”的百分比。

不在未测前承诺节省多少 MiB/FPS，不以漏画或降画质过门。每阶段分列生产代码/CPU/GPU/玩家视觉证据。
Win32 HWND 与跨进程调用的消息泵互等、队列背压、GPU丢失及 helper 崩溃是硬门，不静默丢 draw 或续用旧设备。

## 2026-10-22：X2 的生产参考退出门已关闭

- 产品内 PostFX、UI/backend/cursor、实例缓冲的内部引用与设备生命周期已收敛；
  保持外部 COM 资源的父设备保活，不修改通用引用计数、caster 准入、预算或 GPU 退役规则。
  独立完整 Win32 DLL 构建及同源生产对象探针都已实际执行，不再只有之前的五个 TU 编译。
- 原跨位数门重新执行：remote/reference × normal/invalid 四例各 2548 像素零错，
  新 reference 设备/factory 引用均归零并正常退出；旧 r4 的 15-ref 红门保留，不追认旧产物。
- 新生命周期门用实际生产 UI/PostFX/Reset/双设备/实例 storage；默认和关闭 direct mapping 两配置
  各 3 周期/6 device/60 probe/147 GPU draw 通过。数据来源、失败保留与更精确证据边界见
  [设备所有权收口](../research/2026-10-22-device-owned-com-lifetime.md)。
- **X2 尚未完成**：当前远端仍 `Present=E_NOTIMPL`。下一实施门是复用原
  `Presenter + DxvkSwapchainBlitter + DxvkDevice::presentImage`，接同一 x64 D3D9 颜色输出，
  固定窗口先验多帧真实 Present 与正常退出。独立 WSI demo 不替代这条接线。
- 其后仍按原顺序闭合产品异步/背压/故障退出、D3D9 兼容矩阵，再做游戏画面与 x86 内存 A/B。
  无游戏启动/部署/推送发布，不宣称渲染负载已整体迁走或玩家内存已经下降。

## 2026-10-22：原 Presenter 已接通，受限隔离桌面门 r7 通过

本节覆盖上文历史 checkpoint 的 `Present=E_NOTIMPL` 状态，不追认旧产物。

- 同一个 x86 D3D9 代理和 x64 原 DXVK owner 已接原 Presenter / SwapchainBlitter，
  不再是独立 WSI demo。真正的非输入 Win32 Desktop 上，r7 的 37 个 case 通过；
  包含 remote/reference、诊断开关、连续 8 帧及 4 轮窗口故障组合。
- 诊断读取实际 acquired swapchain image，8 帧共 20384 项像素检查零错；不把 source target
  当作显示结果，也不把 acquired-image 证据外推到 compositor、屏幕 scanout 或玩家视觉。
- Fresh Clear→Present 暴露并修复了 deferred clear 交接缺口；健康 owner 退出先走原
  device idle 排空 CPU submission/finish 尾部。r3–r6 失败与有限根因证据全部保留。
  4 个仍持有资源的故障例是明确 self-fail-stop，不是正常 GPU 析构。
- 共用修改已完成新的独立 x86 DLL 全量构建/no-work：
  `build32_device_owner_presenter_20261022`，34,476,032 B，
  `5A8B5B691BBB18473D975DC3CAB1BEDD3FB641A938D10E8AB5A6FFBA9BECAB8C`。
  该构建配置完整回归已实际完成 116/116，全目标 no-work，源码和受保护DLL均未漂移。
- 下一小批 R1/Q1 已实现资源 ID/generation 复用和独立 Query 生命周期，另有连续 256 次
  Present 排列测试。r8 已编译/实跑：连续256帧、1048576像素零错，
  但主退出等待再次超过5秒，总门失败保留。R1/Q1的r2已通过96轮COM资源复用、
  256次Query及raw61个拒绝/恢复反例；r1的45秒超时仍未解释，不追认旧回执。
  **退出可靠性必须继续定位；r7短组合通过不能盖过r8长循环失败。**
- r9又在8帧诊断Presenter的自然退出复现5秒超时，已经越过`wmain`局部析构。
  保留r8二进制的repeat3诊断得到三份单主线程现场；WCT未给出可解释等待对象。
  后续先取退出栈并定位，随后重跑完整Presenter与独立Vulkan validation，不改超时门求绿。

完整证据、失败保留与范围见[Presenter 隔离回执](../research/2026-10-22-x64-d3d9-presenter-isolation.md)。
产品异步 transport、全 D3D9 / WarVK pass、游戏 x86 VA/commit 和尾延迟 A/B 仍未闭合；
当前代理仍拒绝 Game.dll，不能据此把 X2/X3/X4 或“迁移效果充分验证”标完成。

## 2026-10-22续批：M0账户与退出调查

- 已从旧r9现场取得真正退出栈及匹配Microsoft PDB：卡点在系统OpenGL
  `vCleanupWnd`等待palette watcher，50ms×最多100次。绘制/Present及host局部析构已过；
  watcher由谁引入、初始化阶段另一次超时仍未完全归因，不延长原5秒门求绿。
- 所谓隐式层off对照在High完整性令牌下没有实际生效，真实loader仍加载隐式层。
  因此那些BAAB只保留诊断观察，不算隔离变量对照。Medium子进程在CPU启动阶段报
  `0xC0000142`，必须先修复测试环境并验证实际层装载，再执行独立VVL；不改系统/共享Desktop安全。
- M0 r2实际两进程账户已通过54条native账户断言、96轮资源复用/256Query/319488像素零错、
  8个按身份绑定的采样窗口及1487条实际回执读方变异。dead-slot容量也参与认证，不只看live标记。
  x86四个drained点private稳定在2318336B；x64对应增长12804096B，仍需分解，不能宣称全进程平台稳定。
- 真实destroy变异r2在同源复制TU中故意保留12B死槽容量，准确在首个drained样本拒绝，child自然退出；
  r1则在初始化阶段超时，未到目标且仍记失败。全程不改预算、不删draw、不覆盖玩家DLL。

续批更新：有效Medium CPU启动已通过；实际loader层off对照及真实全空层来源诊断已执行，
后者仍出现相同OpenGL detach退出栈。层过滤不是充分修复。新共享DXSO canonical cache已完成
源码独审、真实编译器CPU88条检查、专用新x86 DLL链接/no-work、M0 r3和三方GPU像素等价。
M0实际192次CREATE仅2次compile/register与190次hit，token容量400B稳定；old/new/remote
各72帧共718848选定像素零错。有限负载复用成立，但唯一shader工作集仍未有界，未证明游戏内存收益。
当前host的R1/Q1 r3也通过，资源/查询反例后恢复、最终逻辑清零和自然退出均经raw独审。
新x86配置内全回归已在restricted-Medium非输入Desktop完成116/116、全目标no-work；
2099项源码与初始raw、产品/天气/玩家DLL全部保全，Job空、Desktop关闭。
回执`x86-shader-cache-regressions-20261022-r1`，不是旧DLL结果复用。
见[共享编译模块checkpoint](../research/2026-10-22-dxso-canonical-module-cache.md)。

当前Presenter r10在首个clear-soak初始化耗时5375ms，超过原5000ms回包门，整轮失败保留；
这是初始化问题，不是该轮坏像素或退出失败。watcher r1/r2暴露的ImageBase表示校验缺口窄修后，
r5已抓到同一watcher退出前存活、退出等待时保留HANDLE已结束但全局TID仍非零；原5秒门仍失败。
初始化分段三轮已完成但未复现旧超时；同EXE surface-owner ABBA已证明本机固定采样下peer窗口
触发活体watcher、host-owned窗口对应认证0，四臂像素/自然退出均过。仍不授予完整显示或旧退出修复门。
实际输入/结果见[初始化和窗口所有权](../research/2026-10-22-x64-initialization-and-surface-owner.md)。
下一门：沿用同EXE的peer故障拒绝/八帧/长负载 → 窗口所有权产品方案（嵌入/DPI/父先退出另验） →
关闭初始化/退出可靠性红门 → 同身份完整Presenter/R1组合 → 独立VVL。
其后必须继续X1产品异步/背压及D3D9兼容，不把M0小负载账户当游戏迁移收益。
详细证据见[内存账户](../research/2026-10-22-x64-memory-account.md)和上述Presenter回执。

## 2026-10-23：受限宿主VVL门推进，X1a进入实际接线

- 同EXE12例续验最终为host 6/6、peer 3/6；三次peer primary-exit超5秒仍失败，未延长期限。
- 独立VVL发现一个真实函数resolver错误，已将物理设备multisample查询归还InstanceFn。
  Acquire/WaitForPresent2并发告警经规范与上游修正核实为旧层误报，不给热路径加锁或关特性。
- 新官方固定VVL365对照完成：旧host仍准确报GPA警告，新dispatch host的256帧与shader8两例
  零warning/error，原像素/退休/时限/隔离收口通过。旧层失败与新版配置失败原样保留。
  这只是两例host-owned窗口诊断；完整C2、窗口嵌入与此前初始化红门没有因此关闭。
- X1a首批限定Clear本地有界入队，Draw及其他调用保持同步保序屏障，保留四队列槽+一worker私有批。
  先验实际Connection CPU反例，再验真实IPC满队列/故障/Draw屏障，不把入队或CPU ACK当GPU完成。
  生产异步owner/非阻塞销毁、全D3D9/WarVK pass和游戏VA/commit/p95/p99 A/B仍为未完成。
- 逐项实际身份与失败边界见[初始化/窗口所有权记录](../research/2026-10-22-x64-initialization-and-surface-owner.md)。

## 一手资料

- [Vulkan 外部内存与同步](https://docs.vulkan.org/guide/latest/extensions/external.html)
- [Win32 semaphore import 不接管 NT 句柄](https://docs.vulkan.org/refpages/latest/refpages/source/vkImportSemaphoreWin32HandleKHR.html)
- [外部队列所有权与同步](https://docs.vulkan.org/spec/latest/chapters/synchronization.html)

进度只由本轮回执更新；本文件是实施计划，不是完成声明。
