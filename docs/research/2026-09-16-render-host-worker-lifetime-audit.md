# P3b2实际样本worker：寿命与失败回执

2026-09-16 05:47，独立CPU实验，不是产品渲染迁移或玩家崩溃结论。
实施前合同/架构图见[WVP1 ADR](../plan/2026-09-16-render-host-sample-envelope-contract.md)。

## 共用边界

`slot_host_runtime`仍只有一份RH1循环与SlotLedger。原P2入口为薄wrapper；新consumer只在同步
ReadPermit/private-copy范围检查WVP1，不持有跨调用指针。SHA正确不代表payload合法；全部身份和顺序还要过门。
生产者固定scope四槽SPSC与I/O worker分离；只有后者创建/等待进程、管道和共享mutex。
sourceFrame允许重复，attempt允许Full/Invalid缺口，RH1 transfer必须从1连续增加；三者不能混用。

## 实际结果

- 最新`AutoTest/artifacts/overnight_architecture_20260916/worker-r13/receipt.json`：**14/14通过**。
  SHA256 `B1B61B34E08403F6B34351419964956FC61726D9D49601655C9B991D6FEE4569`。
- 60个真实IPC lifetime：正常、慢消费、断连、新连接恢复、16正常/断连交替、两方向各16能力拒绝，
  scope/transfer/attempt/source-frame/length五类错误。每个host精确PID/creation、实际退出与容器不存在均复核。
- 287 ACK、302 shared writes、6,318,702bytes。15份已写但无ACK输入没有隐藏：written回执独立于ack，
  Python按实际注入字段重新构造完整80+payload字节并重算SHA。host须在该SHA匹配后才走到对应的payload拒绝点。
- host实际delay1200.1939ms；producer的1000次尝试在其QPC区间内完成，4入队/996 Full；恢复后还有合法新样本。
  本次该批次27.5微秒，仅作为“未等待host”的实验窗口证据；小payload且绝大多数Full，**不是传输吞吐或游戏性能数据**。
- 同一scope正常运行包含1/97/65456bytes、重复源帧和ordinal缺口。失败连接不重放，不复用旧queue；恢复另建nonce/epoch。
- 共用host提取后P2 `shared-r9`41/41通过；定向Python门39/39（其中新worker gate8组）。
  未构建产品DLL，未运行游戏/GPU，未修改live或暂停调试现场。

## 失败尝试保留，未调低门

- R1被`-Werror=misleading-indentation`拒绝，修正明确的join控制流后重建。
- R2未充分隔离依赖首次初始化，计数97→108；R3补原P2同类security/mapping初始化，普通路径闭合，
  能力立即拒绝仍103→107。R4换显式CRT线程退出后仍有103→106，故不能把std::thread称为已证明根因。
- R7/R10同样被句柄门拒绝，不能选一次通过来掩盖。R5/R6/R9为诊断输出，因stderr不空故意不算验收；
  R8是compiler路径录入错误，构建未启动，不构成任何测试成功。
- R11只查询**本实验进程**handle snapshot：新增500/504/508三项均为**EtwRegistration**，并非pipe/mutex/
  mapping/owned-thread残留。NT snapshot与类型查询只在可选AutoTest探针中存在，产品和正常hot path不依赖其私有ABI。
  参考[phnt的ProcessHandleInformation声明](https://github.com/winsiderss/phnt/blob/master/ntpsapi.h)；
  没有复制其业务实现，也没有枚举/修改War3或x32dbg句柄。
- R12/R13将精确host无参数启动单独结算：发生增长的case为image-init **103→106→106**，真实拒绝事务后仍106。
  其余case初始化后稳定，所有16轮门每轮相等。无参数host必须在碰IPC前以2/3明确拒绝，有自己的log/PID/creation。
  这支持“具体image冷启动的有界ETW注册”，但尚未识别具体provider/注册调用栈；不推广为任意系统环境保证。
  不存在放宽允许增量、等待计数偶然下降或丢掉失败连接当暖机的逻辑。

## 仍未完成

worker现在是AutoTest适配器，不是游戏可直接调用的服务；下一步应先设计用户可观察的启动/停止/故障状态、
外部watchdog的产品化归属，再选择有限诊断消费任务试接。不能把blocking lab drain放到Present/render线程。
跨进程GPU句柄、adapter身份、timeline/fence/资源退役、窗口输入、D3D9语义和全量渲染迁移均未开始。
64位进程不会自动让32位游戏不承担资源；在真实迁移/内存测量之前不能承诺VA或爆内存改善。
