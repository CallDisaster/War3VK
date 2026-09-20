# 2026-09-16 无人值守：生命周期收敛与64位进程基础

授权窗口：Asia/Taipei 2026-09-16 01:53 至08:00。07:40起不启动新长任务，08:00前汇总；
自动继续使用当前任务 heartbeat `warvk-2`，每15分钟唤回，不另建项目/子代理。
状态：已收尾，heartbeat `warvk-2` 已暂停。不是全量架构重写，不是闪退/闪烁修复接受。

最终收尾：恢复读时钟为08:46，已超过08:00；未再启动新实现/编译/测试/游戏，只核验与总结。
最后实现checkpoint仍为05:51，之后录制状态交接仅只读审计，不能声称整段时间持续开发。
成果与剩余问题见 `docs/agent-history/2026-09-16-overnight-architecture-results.md`。
6份关键receipt及106项source pin收尾一致；268项原checkpoint文件均不变，新增总结后269项。
`status()`/`nextExport()`终态更新分叉记为未实现待办；无人值守结束，不自动启动下一阶段。

05:47 checkpoint：P3b2 `worker-r13`14/14、39项Python与4-file py_compile通过；已写但未ACK完整SHA闭合。
64位基础暂冻结为CPU lab；后续继续审计生产模块生命周期/错误传播，不自动扩到GPU/产品接入。
05:51收口：status268，198原路径内容不变、原授权11项修改、59新路径，无删除；P1/P2/P3/recorder共6份receipt
106项source pin复核一致，暂停PID与2D4B build/live保持。边界receipt `checkpoint-p3b2-0551.json`。
下一轮优先从`war3_frame_recorder_session`的worker退出/故障结果向`frame_history`消费交接审计；
先找到具体可复现的边界或写清未发现问题，不为完成数量新增Manager或扩大GPU生命周期更改。

05:44 checkpoint：P3b2 `worker-r12`实际14/14通过。冷启动增加3个ETW registration已用self handle快照和
精确host无IPC启动隔离，反复连接句柄不增。下一步补failed-in-flight完整SHA回执及定向gate反例，随后冻结。

05:18 checkpoint：P3b2开始实际适配，共用host提取完成，P2 `shared-r9` 41/41实际回归通过；
P3真实worker尚在实施，不能沿用P2回执宣称有损样本链已过。QPC慢消费前提已在envelope ADR补齐。

## 基线与边界

- 唯一工作树：`dxvk-v1.22-integration-20260914`；branch `codex/v1.22-release-integration-20260914`，
  HEAD `ae890542d766470d1703f5bea7f5b73636039733`。初始209 dirty，逐文件身份见
  `AutoTest/artifacts/overnight_architecture_20260916/baseline.json`。旧默认dxvk树只读。
- War3 PID13216 / x32dbg PID32228暂停现场继续保留；2D4B磁盘DLL不覆盖。
  “取证导致地址空间耗尽”是优先假设，不冒充完整异常/dump因果证明。
- 用户已允许夜间实现、构建和辅助工具，但不做无关安装、系统安全/驱动/LAA改动、重启、
  提交/推送/发布。未完成Water与已搁置原版模型灯不进入本轮。
- 原型默认不链接产品DLL、不自动启动、不读取Game内存、不新开真实游戏。
  CPU构建/测试可在保留的暂停现场旁独立运行，父进程BelowNormal，最多-j2；GPU事务不并发。
- 每个checkpoint更新本计划和开发日志，root CHANGELOG不写候选为稳定。

## 内部工作表（按门推进，时间是预算而非完成声明）

| 窗口 | 工作 | 成功条件 | 状态 |
| --- | --- | --- | --- |
| 01:53–02:40 | 基线、所有者/会话审计、协议ADR与架构图 | 明确实现/保留/禁止边界，先文档后IPC代码 | 已完成首版 |
| 02:40–03:30 | 录制会话交接最小收敛 | 旧consumer不清新请求；状态单一来源；生产CPU正反例 | 02:12完成CPU门，未重建DLL |
| 03:30–05:30 | 32↔64位CPU协议/辅助进程骨架 | 真实跨位数握手、有限消息、错误/退出，未接GPU | P1/P2a/P2b/P2c通过；实际共享41例含48轮寿命 |
| 05:30–07:15 | adversarial协议/生命周期复验与下一条债务 | 长度/序号/会话/断连/背压不绕过；不为了数量全库改动 | 05:47 P3b2实际14例/60寿命通过；后续只读审计 |
| 07:15–07:40 | 源码复审、打包可审核成果 | 文件/测试/未过门可追溯，无后台进程残留 | 未在计划时刻完成；最终交付源码/文档/既有回执，无另制压缩包 |
| 07:40–08:00 | 停止扩张、最终总结、暂停heartbeat | 展示实际成果与风险，不称渲染迁移或崩溃解决 | 总结延迟至08:46后收尾；已暂停，仅文档/只读核验 |

## 首条实际生命周期交接

`D3D9WindowProc -> TriggerMailbox -> FrameHistory owner -> RecorderSession worker`。
当前`consume(oldSession)`使用无条件exchange，能清掉新session的pending请求。
修正为匹配token的CAS消费；测试必须同时证明旧consumer无副作用和新consumer能成功取走。
`FrameHistory::Impl`与worker对Complete/Fault的数字定义也需收敛到共同类型，保留wire编号不变。
不修改GPU fence/资源释放时机、不扩大到其他缓存/渲染算法。

## 64位基础的进入门

先完成 `docs/plan/2026-09-16-render-host-ipc-v0-contract.md`：组件/状态架构图、威胁模型、
版本/位数/身份、线格式与容量、生命周期、超时/背压、所有权和测试矩阵。
第一版只做 CPU echo/barrier/session 的真实Win32↔Win64通信；不宣称已节省游戏内存。
长历史、共享内存、GPU资源共享、D3D9代理/渲染接管均独立后续gate，不可凭接口名字冒充实现。

## 恢复执行

### 05:04 checkpoint（最新，P3b1）

- 先读`docs/plan/2026-09-16-render-host-sample-envelope-contract.md`全部内容。P3b1 codec已完成：
  `sample_envelope.h/.cpp`，80byte头，payload≤65456，scope/sourceFrame/attempt/transfer分别保留。
  最新`envelope-r1/receipt.json`两端56967检查/10000合法样本/独立81byte golden；失败清view永久Fault。
- ProducerInbox构造已改为命名`inbox::Limits{attempts,sampleBytes}`；原测试低计数改用Limits，新增大小准入测试。
  以`envelope::InboxLimits`构造可在copy前拒绝65457；P3a必须引用最新`inbox-r6/receipt.json`（双位131973/118645），
  R5不再覆盖新header。queue占用未变262464，暂停consumer仍4/1996；源码没有IPC、Game、GPU接线。
- status259：198原路径不变/11旧改动/50累计新增。D3D9两个2D4B、暂停13216/32228仍保留。无新helper进程。
- **下一步P3b2可执行路径**：不要复制RH1状态机。把`slot_host_main.cpp`现有runner提为`slot_host_runtime.h/.cpp`，
  保留极薄P2 wmain wrapper；新P3 wmain提供同步payload consumer（begin固定scope、consume只借用private-copy）。
  在原Publish的copyPrivate/SHA之后、finishRead/ACK之前调用；失败仍走原retire/reader结算，P2 callback为空保持原义。
  新P3 consumer严格校验WVP1并记录sourceFrame/attempt/transfer；真实capability=6，旧P2=2，实际双向错配要测。
  需要同步更新standalone Meson和P2 gate的host sources/pins，重跑实际P2 41例；若改底层transport/child则P1也重跑。
- P3 test-client应让一个I/O worker独占pipe/SharedSlots/child事务，producer只访问queue，不做文件/进程/握手。
  慢host在首个Publish的私有copy消费点记录delay-start/结束并flush，注入固定约1.2秒(<5秒I/O期限)。
  可由**测试协调线程**读取已CreateNew的host log确认实际delay-start后放行producer固定批次，不能由producer轮询日志。
  `OwnedChild`日志创建为FILE_SHARE_READ，允许协调器只读；不要新增全球输入/不受限命名同步对象。
  QPC开始/批次结束/延迟结束可证明实际跨进程重叠（先查一手QPC跨进程依据），且queue满后恢复新合法样本仍可接收。
- 断管案例在第3份已验证payload处中断ACK，host-valid不当client-ACK；记2成功/1failedInFlight/queued剩余，
  worker requestStop，producer close/join后统计，精确host/OS完成后释放。再另建nonce/queue/host验证正向恢复。
  不在线重连或补跑同样本，Python逐条独立核scope/两种序号/bytes/SHA及人口代数。23秒外层watchdog触发即失败。
- 07:40停止新增，08:00前总结并暂停；剩余约3小时，不为赶进度接入真实游戏/64位GPU。

### 04:49 checkpoint（P3a复审）

- 最新P3a proof改为`inbox-r5/receipt.json`：SHA `246612DE20EA2395635B612A9BE563E93DCB6881C18E8723E10EFB81594DCDE6`。
  两位检查132321/117909，50000尝试各7504/3901发布、Full42496/46099、96620596/84779890bytes，固定暂停门仍4/1996。
- 只加强测试/gate：第二轮Core用不同nonce/map/device且逐样本校验新scope；旧R4同scope新对象不足以称新连接身份恢复。
  核心header不变。新6组Python/py_compile通过；不要再用R4校验当前test/gate。状态仍254，其他边界沿用04:46。
- 下一步保持P3b：先读下面04:46的RH1连续transfer与真实sourceFrame/attempt区别，写envelope/容量/ACK/worker退出合同。
  不跳过此语义边界，不把本地消费者暂停测试说成跨进程慢host，不将任何CPU ACK等同GPU完成。

### 04:46 checkpoint

- P3a先ADR/架构图，`docs/plan/2026-09-16-render-host-producer-inbox-contract.md`；header-only `tools/render_host/producer_inbox.h`。
  四槽私有SPSC，不是跨进程atomic；两个固定线程，各自唯一API责任；有界copy、Full不覆盖、ordinal丢失显式统计、
  stop不是结束、close排空重查最后head、两线程join后统计/析构。无游戏接线或帧采集热路径变化。
- 最新`inbox-r4/receipt.json` Win32/Win64各131825/116117检查；queue262464bytes、Sample65584bytes，32位原子lock-free。
  实际暂停consumer至producer完成/join：2000尝试、4接收/1996Full；恢复后内容全部正确。
  各50000双线程尝试，分别7380/3453接收且全字节验证、Full42620/46547，含消费线程请求stop后排空。
  数值受调度影响，不是FPS或采样率比较。普通/数组/对齐C++new guard，但不是任意malloc拦截，也没跑Tsan。
- 新6+旧25=31 Python门、2-file py_compile/diff通过；254dirty：198原路径不变/11旧修改/45新增、无删除。
  P1/RH1生产实现未改；独立Meson加inbox CPU test，根Meson未改。R3是调用的compiler路径笔误失败，保留，R4才是最新proof。
- **下一步P3b先补合同再实施**：现有RH1 SlotLedger.reserve要求`frame==lastFrame+1`，它是连续transfer序号。
  P3a的sourceFrame可重复，attempt ordinal含Full/Invalid gap，绝不能直接写入旧frame或重编号后丢掉来源。
  先设计有界payload envelope（sourceFrame/attempt/scope/bytes与transfer序号分别保存），明确扣header后的最大payload，
  host实际解析/验证，再接I/O worker；不要只在client日志补标签而host完全没验证。
  真实慢host/断管与ACK逐样本关联、队满后恢复、stop/join/失败人口闭合；P3a暂停本地consumer不代替此门。
  保持lab-only；P1/RH1如有实现变化重跑相应真实门，不让旧receipt冒充新源码验证。07:40止新任务，08:00前收尾暂停heartbeat。

### 04:33 checkpoint

- 生产控制租约已完成小合同，ADR：`docs/plan/2026-09-16-recorder-control-lease-contract.md`。
  header`war3_frame_recorder_control.h`只含纯CPU Authority/不可复制一次性Lease；Control/History实际接口和worker接线已改，
  外部默认nullptr、schema不变。现有controlMutex单authority；join后release，image先authority校验解锁再registry精确绑定。
- 新`AutoTest/run_recorder_control_lease_gate.py`可独立复现，不用Ninja；最新`recorder-lease-r2/receipt.json` 15/15 CPU入口，
  核心9766/生产Control723/disabled3/memory45/内存注入86/默认profile10组合。争抢4线程2000尝试255成功0错误。
  真实history/evidence/control-plane/inputs四TU语法通过，未构建产品DLL；原有focus unused-parameter warning仍记录。
- Python111定向门、3-file py_compile/diff通过。249 dirty：198原路径不变/11旧修改/40累计新增，无删除。
  玩家/build32仍2D4B，暂停War3/x32dbg未触碰，helper/build进程0。不是闪退修复/游戏退出验收。
- **下一阶段P3**先ADR：如何把lab的阻塞RPC隔离在专属I/O worker，使真实producer只能有界copy/try-publish，
  队满丢样但显式计数，不能等待pipe、mutex、SHA、文件、child退出。不得把P2的共享mutex/RPC直接放render线程。
  先选明确单producer/单consumer线程合同与稳定不可变payload，禁止raw pointer/GPU Rc跨wire；需要exact sequence/gaps、
  map/device/connection身份、epoch边界、cancel/stop/join、迟到ACK、背压后正向恢复和预算。
  仍独立CPU实验室、不接游戏；如实现则需真实核心/线程+受限慢host/断管测试，不以静态字符串冒充非阻塞。
  若P3范围过大，停在设计与可测核心，不仓促迁移GPU/长历史。07:40停止新增长任务，08:00前总结并暂停heartbeat。

### 04:07 checkpoint

- P2c已完成当前实际CPU门：`shared-r8/receipt.json` **41/41**，双位wire各23056检查。
  48个真实共享容器寿命在同一Win32 producer，4类各12次；各轮句柄基线精确相等、对象名消失、精确child退出，
  24个Fault后后续合法会话仍恢复。180合法样本/564成功控制/204write/4829208byte，逐轮Python重建核验，不称性能或长期内存收益。
- 补在途Writing父退出、非法状态顺序和slot真正复用后的迟到key。Partial-write R8已是真实mapping只memcpy前半，
  不是R5/R6/R7的全长零尾模拟；保留早期证据边界，不改旧receipt。实际生产共享槽/host实现本阶段无变化。
- 判定器RH1 15组+RH0 10组=25通过；相关py_compile/diff通过，245 dirty仍202原路径不变/7旧修改/36新增。
  P1最新仍R16，公共source未变。所有helper已结算，玩家/build32仍2D4B，暂停War3/x32dbg未动。
- **下一条生产CPU交接（已只读定位，未实施）**：`FrameHistory::runRecorder`以bool `localRecorder=true`进入
  `evidence::Control`/`FrameHistoryControl`，`ClaimLocalRecorder/ReleaseLocalRecorder`目前是无身份bool占用。
  现有实际Release仅swapchain析构路径且先join；不声称已发生误释放或这是闪退根因。
  可在同一控制Mutex owner下用一次性、不可复制、精确代际的控制租约收拢授权，避免未来旧owner/裸true调用混用。
  不要另起并行manager，不把录制session token和控制owner代际混为一谈。先制定调用图/锁序/失败/退出合同，
  查全调用点与静态测试，再实际核心正反例和编译TU语法；不改GPU资源寿命/热路径/导出格式。
- 如果新交接风险超出可验证边界，则登记债务，转P3有界非阻塞producer协议设计，不急着接游戏。
  07:40停止扩张并汇总，08:00前暂停自动化；不要把现有lab阻塞RPC或hash/mutex直接塞进render线程。

### 03:51 checkpoint

- P2b独立RH1/WVS1协议及真实32位writer→64位CPU consumer完成；先ADR后代码，协议/时序图见
  `docs/plan/2026-09-16-render-host-shared-slots-wire-contract.md`。共享池256KiB，host只读view+私有64KiB copy。
- 当前actual proof：`shared-r5/receipt.json` 27/27，双位codec各23056检查、72byte golden，Python独立byte/SHA；
  正常、fragment实际2674writes、取消/重复/全key/部分写/mutex abandoned/timeout/退出/同名对象均闭合。
  9新+10旧=19组Python判定器，相关py_compile/diff通过。R1–R4中间证据保留，R5才匹配当前共同owner源码。
- 首次C++异常2句柄初始化已单独测量，8次之后再64次无增长；真实事务仍须精确句柄相等，不能宽容差。
- 共用`win32_security.*`、low-level ReadWindow及channel名字加入后P1 R15过；
  `OwnedChild`最终drain又收紧为必须process signaled，因此**P1最新是R16**，16/16含64 lifetimes。
  内核cancel/final-exit drain允许由外层watchdog判失败，仍不适合游戏热线程，不能改成超时后丢owner。
- 源码目录新增`shared_slots.*`、`slot_wire.*`、`slot_channel.h`、`slot_host_main.cpp`；测试/显式gate在AutoTest。
  standalone Meson有target，根Meson未接；最新P2b编译serial BelowNormal、PE32/PE32+核验，无游戏/DLL/GPU。
- 当前245 dirty（原209中202 SHA不变、7旧变化、新增36），玩家/build32仍2D4B，暂停现场未触碰，无helper残留。
- **下轮优先**：同一个producer重复创建/关闭真实mapping+mutex+host，逐轮句柄/对象消失/合法新会话恢复；
  加入Writing lease存在时的父退出、更多Hello/End/Close/Reserve非法顺序、旧generation在slot实际复用后的迟到Publish。
  保持fault就终结连接、无暗中重试/阈值放宽；P1的64管道会话不代替此门。
  P2b没有迁移游戏长历史，P3前还需有界非阻塞producer/背压/缺失计数合同和CPU payload结构，不能将lab RPC接render线程。
- 如上述基础充分且仍有时间，再沿架构计划选择另一条现有生产CPU owner交接做小修正，先红后绿；不要扩大到GPU释放重构。
  07:40起止新长任务，汇总整晚并暂停`warvk-2`，08:00后不新增实现。

### 03:14 checkpoint

- P2a先ADR后核心，合同在`docs/plan/2026-09-16-render-host-slot-ownership-contract.md`。
  新`slot_ledger.h/.cpp`、`AutoTest/test_render_host_slot_ledger.cpp`、`run_render_host_slot_gate.py`。
  真正核心两种位数各116269检查、10000合法元数据记录；最新`slot-r4/receipt.json`，golden与Python独立相同。
- Key只是未信任描述符，ReadPermit为不可复制/一次性CPU凭据。取消隔离而非即刻复用；旧ACK/旧epoch/重开不破坏新租约。
  未创建共享映射或复制跨进程样本；P2a不是物理共享内存/游戏内存收益证据。RH0保持原样。
- 当前232 dirty（原209中202不变，7旧改动，新增23），未删除、未改稳定日志/玩家DLL/暂停进程。
- 下一步P2b必须先明确**新wire版本**、四个固定slot映射和每槽Windows mutex。不要让RH0 SampleEcho变成隐式资源API。
  可复用已验证的低层overlapped与owned child，但若改共同传输层要重跑P1全16例；不得沿用旧R14称新源码通过。
  Windows映射使用Local、nonce名称、logon SID DACL、already-exists拒绝，无execute权限、固定256KiB总数据。
  mutex必须有限wait，WAIT_ABANDONED拒绝/退役，复制到私有buffer后校验，不以取消或断管当写者/GPU结束。
  优先保持实验室CPU范围；时间不足则停在验证充分的边界，不急着接游戏。

### 02:55 checkpoint

- P1当前源码实际证据：`transport-r14/receipt.json`；16场景全过，其中64连续helper生命周期43成功/21错误nonce，
  每轮句柄不增长、各helper已结算。真正Win32客户端/Win64宿主，不是同进程模型。RNG依赖双位数另64次稳态检查通过。
- 全部P1源码在`tools/render_host/`，测试`AutoTest/test_render_host_transport.cpp`、`test_render_host_random.cpp`、
  `run_render_host_transport_gate.py`、`test_render_host_transport_gate.py`。10项Python门、3-file py_compile/diff通过。
  README/独立Meson/协议均更新；没有根Meson改动、产品DLL构建或游戏运行。不要重复下载编译器。
- 前期R1–R13失败/中间证据保留；句柄初始化问题经过独立RNG检查和64轮稳态测试，不以随意放宽阈值处理。
  传输的内核取消drain仍可能阻塞，仅实验室外层进程看门狗可终止；不得把同步lab接口接入渲染线程。
- 当前227 dirty：原209中202不变、7项阶段一改动；累计新增18，无删除。玩家/build32仍2D4B，
  暂停War3/x32dbg未触碰；无编译/RH0残留。根CHANGELOG不改。
- **下一工作P2必须先ADR**：有限槽的owner、byte range、slot generation、connection/map/device/frame、
  publish/CPU-consume/release/断连retire、迟到ACK与ABA、预算耗尽与正向恢复。先实际所有权核心测试，
  再决定是否时间足够做独立共享映射。RH0已冻结资源字段0，不直接拿测试echo当资源传输。
  跨进程ABI不能使用native pointer/size_t/std::atomic对象布局；跨进程同步要先查Microsoft一手API合同。
- 到07:40停止扩张，以全部checkpoint形成当晚总结并暂停heartbeat，不等到08:00才开始收尾。

### 02:12 checkpoint

- RecorderSession/TriggerMailbox现由真实核心1734检查覆盖；先红后绿，GPU寿命未改。
- RH0 `tools/render_host/protocol.*`与独立meson入口已实现；32位GCC、64位Clang分别41586检查，
  同一golden与独立Python逐字一致。receipt：`AutoTest/artifacts/overnight_architecture_20260916/protocol-r1/receipt.json`。
- 便携x64编译器：`E:/Dev/Toolchains/warvk-rh0-llvm-mingw-20260616/portable/llvm-mingw-20260616-ucrt-x86_64/bin/x86_64-w64-mingw32-clang++.exe`。
  发布zip已核size/SHA；不需再下载/更改PATH。原i686GCC仍E:/Dev/MinGW/bin。
- P1尚未实现：请先核取消不等于完成的所有权合同，不写栈上OVERLAPPED超时即释放的代码；
  保持真实管道身份/权限/整包期限，先建transport测试再考虑共享槽/GPU。
- 不要从“两个位数的可执行文件都通过”推断已进行跨进程通信或节省游戏内存。
- 02:17复审：增加Closing→Closed精确回执I/O完成边界；最新proof为`protocol-r2`，两端各41668检查，
  不再用R1当最新源码证据。相关Python140通过，原209中202项不变、7项变化、新增9项，总218。
- 编译工具zip和展开约0.87GiB，E盘尚有充足空间；无需重复下载。没有实际IPC管道/宿主进程尚在运行。

每次唤回先看时钟、此表与开发日志开头最新checkpoint，检查当前执行进程和文件变化。
若遇权限/工具阻塞，不无限重复；保留证据并切换独立离线工作。
07:40后只收尾，报告路径、差异、实测/模型边界和下一步，暂停`warvk-2`。
