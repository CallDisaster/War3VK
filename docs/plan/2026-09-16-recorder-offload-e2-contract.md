# Recorder E2：真实32→64 CPU实验合同

状态：2026-09-16 E1通过后主线程冻结，**不是实测通过报告**。不链接产品，不启动游戏/GPU。
总范围沿用[CPU外置阶段合同](2026-09-16-recorder-cpu-offload-contract.md)。E2完成后先总结并暂停监督，
不直接把实验室的阻塞取消/析构接入DLL；E3须独立风险与验收合同。

## 开工与所有者

E1回执 `AutoTest/artifacts/recorder_offload_e1_parent_20260916/final-receipt.json` 的17份身份全部匹配；
298 dirty、相关游戏/编译进程0。E2另有Git安全备份ref
`codex/backup-before-recorder-offload-e2-20260916` / `454ba2b8b360694e0b65c6f2a21c28c54e5fce3d`，
原字节/299文件/index/bundle在 `D:/WarVK-Backups/20260916-recorder-offload-e2/`；旧备份不覆盖。

```mermaid
flowchart LR
  P[32位实验producer: 确定性Event] --> Q[MPSC 8192槽 约3.5MiB]
  Q --> W[唯一worker: trigger cut/编码/IPC]
  C[控制线程请求 trigger/stop] --> W
  W --> S[现有4×64KiB共享槽与私有副本]
  S --> H[64位唯一HistoryStore: 最大98MiB]
  H --> R[Seal后遍历/流式SHA及内存回执]
```

- 不重写Event/codec/store/slot ledger，不修改E1已验证核心。32位可执行文件不能链接HistoryStore；
  只保留入口、最多160条编码批次和固定共享窗口，不能累积Event或ACK历史大vector。
- 现有OwnedChild创建Job、精确进程句柄/PID/creationTime、ACL/nonce/只读host映射、private copy→SHA→
  consume→finishRead→ACK顺序不变；实际退出及映射/锁容器消失由主线程独立核验。
- 本阶段只传确定性实验Event，`recorder_lab_fixture.h::Make(source)`为C++来源；绝非游戏录制。
  完整位数、坐标/opaque bits保真，不赋予原始owner/data任何指针权限，不进行GPU导入。
- producer调用只接触MPSC，worker独占pipe/mapping/child与packet ordinal。进入停止态后关闭准入，
  实际join所有producer后排空队列，计数固定才发Seal。任一错误退役事务、关闭队列，不能重试同session。
  lab owner可真实等待退出；外层进程watchdog触发属于**失败**，不能当正常取消成功。

## 外层协议与应用终态

- RH1新增唯一profile `SharedCpuSlots(2)|RecorderEvents(8)=10`。原2/6保持原合同；14等混合profile拒绝。
  握手同向/反向必须完整精确匹配，不以subset测试放宽。实际测试E2↔P2、E2↔P3均拒绝。
- slot的key.frame在此专用profile内是WVE1 **packet ordinal**，不是游戏frame。nonce/map/device/
  generation/slot/bytes继续由真实ledger和只读permit授权；WVE1.header.ordinal必须等于key.frame。
- 外层Begin为固定map=11/device=17，每连接仅一epoch。之后共享槽先WVE1 Begin、Data若干、Seal一次。
  本fixture session固定 `0x1020304050607080`，每条Event的map/device/session及逐字段内容必须匹配fixture。
  生产协议nonce仍每次新随机，不因fixture固定session复用连接或容器。
- `SlotPayloadConsumer::end/close` 为主线程新增默认空回调；旧consumer不变。End先确认ledger无在途
  再检查应用已Frozen/Seal；Close还须成功End。任何回调拒绝都走原retire异常路径，不ACK成功。
  Retire只表示CPU槽退役，不能设置endSeen/closeSeen或完整历史。
- Host在RunSlotHost返回之后输出一条recorderHost JSON；只有runtimeExit=0、成功End/Close、
  store仍Frozen、遍历逐字段正确及lost=0才有cpuComplete=true；这仍不叫图像或完整GPU取证包。
- 验证失败不得输出可用的成功digest；loss>0但合法Seal允许遍历诊断内容，cpuComplete必须false。
  借用permit/data/view不逃出consume。大数组只能位于64位HistoryStore私有堆，不映回32位。

## Trigger线性化（不能只补一个atomic）

控制线程只发布triggerRequested release布尔及请求QPC；**不自行选择并迟到发布旧cut**。
worker在下一批pop/发送之前 acquire请求，第一次看到请求时以publicationCut采样并固定cut；
它独占WVE header，之后每包重复该cut。因为cut读取在worker此前发送之后，不能倒退重分类。
cut包括已claim但未完成写入的记录；consumer不能跨越该洞。记录requestedQpc/appliedQpc及真实cut，
不把worker应用时刻冒充按键时刻。当前E2 first-pass用单一真实producer控制确定性内容；
多生产者基本正确性沿用E1实际并发门，不声称E2已覆盖游戏多模块交错。

normal/small在pre+160成功事件后通过测试协调屏障暂停producer，worker应用trigger后再放行post区；
屏障在测试coordinator，绝不塞入tryPush。slow允许worker被host消费延迟堵住，producer继续尝试并有界拒绝；
记录尝试窗口QPC并与host实际delayStart/End严格比较，不以“睡几帧肯定完成”替代证据。

## 共享输出合同（本批两代理据此独立工作）

纯Python分析入口 `analyze_case(parent, host, transport) -> dict`：返回
`{ok:bool, failedChecks:[str], cpuComplete:bool, gameMemoryBenefitMeasured:false}`。
host在e2-to-p2/e2-to-p3场景为None，其他必须为dict。不能信任输入中的ok字段。
CLI可 `--receipt <new-result-input.json>`，只读输入，输出JSON到stdout；不创建/覆盖artifact。
外层输入schema=1、scope="RECORDER_OFFLOAD_E2_CPU_LAB"、cases列表，每项只有parent/host/transport；
递归duplicate-key/NaN/Infinity拒绝，所有布尔严格bool，计数严格非负int（bool不能冒充int），界限u64。
允许保留额外字段便于后续只读诊断，但不能由额外字段替代任何必需字段或绕过判定。

所有memory对象精确必需字段：valid(bool)、win32(u32)、privateBytes、peakPrivateBytes、workingSetBytes、
usedVirtualBytes、privateVirtualBytes、mappedVirtualBytes、regions（其余均u64）。
成功查询必须valid=true、win32=0、regions>0，peakPrivateBytes>=privateBytes；不能把缺失/无效作为0。

parent必需字段：

```text
mode bits nonce map device session capacity
attempted accepted lost popped queued ackedEvents sentPackets ackPackets trigger
triggerRequestedQpc triggerAppliedQpc qpcFrequency producerStartQpc producerEndQpc
workerJoined hostSettled containersGone hostPid hostCreation hostExit fault completed
ingressBytes poolBytes handlesBefore handlesAfter
memoryBefore memoryActive memoryAfter
```

host（现代helper）必需字段：

```text
recorderHost bits mode session state nextOrdinal lastSequence trigger capacity
accepted attempted lost evicted retained reason storageBytes wireError storeError
runtimeExit beginSeen endSeen closeSeen visited firstSequence lastVisited digest
contentValid cpuComplete delayStartQpc delayEndQpc qpcFrequency
memoryBefore memoryAllocated memoryActive
```

state为实际StoreState数字0 Empty/1 Recording/2 Frozen/3 Fault；wireError/storeError为真实枚举数字，
wireError可以记录consume的外层Decode错误，storeError仍来自HistoryStore。mode为helper启动参数
normal/slow/disconnect，不伪装成parent注入名；现代negative通常host.mode=normal。
visited=0时firstSequence/lastVisited=0、digest=""、contentValid=false；无成功遍历不造digest。
对正常非空Frozen，遍历数=retained，firstSequence..lastVisited连续；SHA-256小写64hex，
计算顺序为这些Event的WVE1 **392-byte Event部分**串联，不含80-byte Header、不含C++padding。
用有界批次流式SHA，不生成另一份98MiB副本。Python用独立struct（不是读取C++源码或调用native）
逐条生成fixture并hashlib累计，核验host最终digest及人口。不能把FNV/摘要提示当完整SHA。

transport为现有RunSlotHost原样summary（summary/bits/peerVerified/state/reason/fault/stage/win32/
accepted/readersDone/viewProtection/poolBytes/copies/copiedBytes/abandoned/mutexTimeouts/reads/writes/
cancelRequests/cancelCompletions），现有字段含义不改。成功host映射PAGE_READONLY=2，poolBytes=262144。
进程物理退出、容器消失及raw身份由主线程gate重算，不靠analyzer自行宣称物理证明。

## 固定场景与门

共同：bits 32/64、nonce32个小写hex、固定map/device/session，ingressBytes=3670400、poolBytes=262144；
attempted=accepted+lost、accepted=popped+queued、ackedEvents<=popped，ackPackets<=sentPackets；
所有终态workerJoined/hostSettled/containersGone=true，handlesBefore=handlesAfter；hostPid/creation>0。
现代host的accepted==lastSequence，retained==accepted-evicted，storageBytes==capacity*392。
所有成功传输需readersDone/peerVerified；不允许abandoned/mutexTimeouts或未结算cancel；外层watchdog不可接受。

|parent.mode|结果及附加门|
|---|---|
|normal|cap262144，pre196608，trigger196768，总accepted262304，retained262144/evicted160；lost0，cpuComplete true|
|small|cap8192，pre6144，trigger6304，总accepted8352，retained8192/evicted160；lost0，cpuComplete true|
|slow|cap8192，lost>0；触发cut=accepted、无post，retained=min(accepted,6144)；合法Seal/End/Close但cpuComplete false|
|disconnect|helper第3次Data返回InjectedRecorderDisconnect，事务failed，cpuComplete false|
|missing-seal|不发WVE Seal便End，reason=RecorderMissingSeal|
|late-data|成功Seal后再发合法Data，reason=RecorderStore|
|wrong-ordinal|第1 Data仅改WVE ordinal使其不等于slot key.frame，reason=RecorderLease|
|wrong-session|第1 Data改Header和Event session，reason=RecorderIdentity|
|wrong-scope|第1 Data改Event.key.mapEpoch，reason=RecorderIdentity|
|bad-kind|第1 Data改Event kind=0，reason=RecorderWire|
|seal-totals|WVE Seal统计自身闭合但accepted与host实际不等，reason=RecorderStore|
|p2-to-e2 / p3-to-e2|现代helper握手收到2/6，reason=HelloContract；不得分配历史|
|e2-to-p2 / e2-to-p3|旧helper收到10，reason=HelloContract；host字段为None，不伪造现代回执|

normal/small/slow：parent completed=true/fault=false，hostExit/runtimeExit=0，transport closed/None/none；
其余：completed=false/fault=true、hostExit=2（协议拒绝）、transport fault及上表精确reason，
现代helper runtimeExit=2且cpuComplete=false，不能因此前Seal通过输出成功。
正常还要求ackedEvents=accepted=popped、queued0，host/parent的session/capacity/trigger/accepted/attempted/lost相等。
slow须真实QPC区间 `delayStart <= producerStart < producerEnd <= delayEnd` 且频率相同正值，证明
consumer实际未继续时producer调用仍返回；不是FPS或尾延迟基准。

内存：normal host memoryAllocated.privateBytes - memoryBefore.privateBytes >= storageBytes - 2MiB；
parent active-before private/usedVirtual增量各<=16MiB，after-before private增量<=8MiB，内存下降保留负差值。
这些是实验fixture门，不是固定OS税或所有硬件承诺；门失败报告真实值，不抬阈值。仍不证明游戏节省了98MiB。
maximum gate须实际遍历全部retained并校验SHA，不得allocation-failed skip。再用新nonce的小正常事务证明恢复。
主线程还须重跑受影响旧RH1 P2/P3门，确认新增回调没有放松旧profile或破坏退出。

### 实测后的实验启动隔离（不放宽事务验收）

`handles-normal-r2`最大历史数据正确，但进程句柄110→111，故失败。
`handles-idle-r1`在相同依赖初始化后**不创建recorder/queue/worker/应用连接**、只空闲55秒，
同样110→111：新增Event和入口位于ntdll的Thread、减少一个旧句柄。未断言具体Windows子系统根因。
因此最大历史场景现在把同样55秒无事务窗口单独输出environmentWindow（QPC/频率/句柄前后），
之后才取memoryBefore/handlesBefore并启动真实事务；不是冷启动耗时或冷启动零增长证明。
短场景窗口为0。睡眠不是退出证明；真实事务的handlesBefore==handlesAfter、实际join/进程退出、
精确容器消失及原有内存门全部保留。最大场景外层watchdog130秒包含启动窗口，实际协议25秒等待不变。
原失败回执不改成通过；没有关闭观察到的系统句柄或修改系统设置。

独立oracle另收紧control ACK/copy字节代数、首Data拒绝零commit、第3Data断连人口以及每连接nonce唯一性。
原threshold不变；native聚合器仍独立核验每个slot/ordinal/generation和raw文件。

## 分工与写入白名单

- 主线程：本合同、process_memory_probe.h/.cpp、recorder_lab_fixture.h、slot_wire.h、slot_host_runtime.h/.cpp；
  后续32位lab producer、聚合gate、原门复验和总日志。这些公共声明代理只读。
- Kimi：仅新增 `tools/render_host/recorder_host_main.cpp`、
  `docs/agent-history/2026-09-16-dsh-recorder-e2-host.md`。现代helper argc5（前三参数沿用RH1，argv4 mode）；
  返回RunSlotHost实际exit，返回前Print一条完整host回执；未开始/失败分配memoryAllocated可与before相同但不伪称分配。
  使用主线程memory helper；real Begin后记录allocated；Seal后遍历/流式SHA；RunSlotHost返回后再裁定成功。
- Gemini：仅新增 `AutoTest/analyze_recorder_offload_e2.py`、`AutoTest/test_analyze_recorder_offload_e2.py`、
  `docs/agent-history/2026-09-16-dsh-recorder-e2-analyzer.md`。标准库，纯synthetic测试；覆盖正常/失败/丢失、
  bits/profile/不闭合、digest错误、memory无效/超门、QPC缺覆盖、缺Seal/退出伪成功、duplicate-key与bool-as-int。
- **所有源码/测试/docs只能apply_patch；不允许Python/PowerShell字符串替换写源码。工具缺失时停笔报告，
  不能换脚本绕过。** 两方无编译/native/Ninja/游戏/部署权限；不改旧文件/协议/阈值/总日志，不新开第三实现者。
  各自artifact仅可新建ignored目录 `recorder_offload_e2_kimi_20260916` / `recorder_offload_e2_gemini_20260916`，
  不删、不覆盖既有证据；结束停笔等待主线程，不自动跨到E3。

## 一手依据与含义

[GetProcessMemoryInfo](https://learn.microsoft.com/en-us/windows/win32/api/psapi/nf-psapi-getprocessmemoryinfo)、
[PROCESS_MEMORY_COUNTERS_EX](https://learn.microsoft.com/en-us/windows/win32/api/psapi/ns-psapi-process_memory_counters_ex)
定义PrivateUsage为进程private commit；不是显存/常驻集/全部VA。
[VirtualQuery](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualquery)
用于本进程冷检查点的地址区间统计，任何部分查询失败均invalid。worker/host测量只在冷检查点，
不为每draw增加扫描。CPU协议与GPU fence继续分离。
