# 真实游戏输入调查与共用固定功能编译器

日期：2026-10-23。当前树 `codex/v1.22-main-tree-20260920`，HEAD
`a2054a9776e40edc3f01f6f390c06037965b8ef2`。在既有 dirty 上继续，未提交、推送或发布。

## 目标与架构选择

本批响应作者“开始实际游戏渲染相关推进”。不另写第二个 Vulkan 渲染器：
x86 保留游戏接入、D3D9 代理与受限数据上传，x64 复用原 DXSO/DXVK 的编译、资源、
上下文、提交和退休。跨进程消息是有类型、有顺序、带资源 ID/代际/有效期的数据和操作，
不是 memcpy 游戏指针、COM 对象或 Vulkan handle，也不是逐帧压缩画面传输。

当前开发代理不能运行完整 Game.dll，保护门保持关闭。已有小型可编程管线实验不覆盖
Warcraft 的固定功能状态、Factory 能力协商及完整纹理语义。本批先把真实入口需求变成
可检查输入，并让同一生产固定功能编译器可在两种位宽独立执行。

## 已实施的数据链与所有者

1. `d3d9_fixed_function.h/.cpp` 提取两个 CPU-only `CompileD3D9FixedFunctionShader`
   入口；原游戏 wrapper 仍执行 Dump/register，缓存与 GPU-skin 不预注册规则保留。
   不复制/改写编译算法，不把 `D3D9DeviceEx`、游戏 Hook 或第二套资源生命周期搬进工具。
2. `d3d9_migration_census.h/.cpp` 的定长统计接到真实 `D3D9DeviceEx`：48 个 API
   入口、四种 draw、三种纹理请求、VS/FS 配置各至多 32 个唯一样本。
   配置 `warvk_d3d9_migration_census_dev` 默认 false；显式诊断构建使用现有 device 锁，
   不新增并行锁体系。Draw/API 与 Texture/API 各自同锁，快照序号与按值复制同锁，日志锁外。
3. 计数表示调用尝试；不证明资源创建成功、GPU 完成或纯游戏来源。
   内部 WarVK 调用可能进入相同公开入口，来源标 Unknown；bound shader handles
   不冒充 `UseProgrammableVS()` 的实际选择。未知与已知零分开。
4. `analyze_d3d9_migration_census.py` 严格校验全部字段、计数恒等式、按 device/sequence
   的单调性与不可变样本前缀；拒绝损坏尾条，而非退回旧快照。容量丢失明确报告。
5. 双宽 `warvk-ff-key-compile.exe` 使用上述同一生产源码，将捕获的配置编译成原始 SPIR-V；
   输入是同一构建的开发 fixture，不是冻结 IPC ABI。目录/文件拒绝覆盖。

诊断组件实际大小 6064 B；CPU 测试窗口内额外堆分配为 0。这不等于 Logger、
整个游戏或完整生产路径零分配。诊断配置强制 device 锁，不能用本轮数据报告 FPS 收益。

## 构建与离线验证

- fresh x86 DLL 与 x64 FF 工具构建、目标 no-work 通过，BelowNormal / `-j2`。
- 构建前后 source/protected 身份一致；独立只读复核全部 1527 项构建源 0 不一致。
- 六项 Python 定向测试首次冻结为 109/109，不是全库静态或 Meson 回归。
- 后续 census-off fresh DLL 构建/no-work 已通过，见下文控制门；它仍不是发布配置组合验收。

构建回执：`AutoTest/artifacts/native-game-input-build-20261023-r1/receipt.json`。
Python 回执：`AutoTest/artifacts/native-game-input-python-20261023-r1/receipt.json`。

| 产物 | 大小 B | SHA-256 |
|---|---:|---|
| 诊断 x86 DLL | 34521088 | `FC7CA86897EA835D2F14DA945A611855EB6940907971E762611E92091F032DF5` |
| x86 FF 工具 | 5423104 | `E9762DBC002861D0CC6DD0D25053FC4F871B4E0FD625CCEEB63D3B532DDBCCC9` |
| x86 census 测试 | 622592 | `A2732475BE2B03345990996C6098A722A364D7979EF17897451D783B29CE9455` |
| x64 FF 工具 | 5246976 | `3F527B1E719905B1BCB84D1A9514EDCD7AD011DE839F1B107A4E05AAF6B19E8A` |

## 第一轮真实游戏进程调查：有原始输入，没有进图证明

`AutoTest/artifacts/native-game-census-20261023-r1/receipt.json` 保留失败。
在新建沙盒副本、restricted-Medium 非输入 Desktop 上，以原 `war3.exe`/`Game.dll`
和同字节 LostTemple 地图启动；没有覆盖玩家 DLL、共享配置或发送全局输入。

- 游戏 PID 34828；有四条合法 census，最后一条为 Present 1024。
- 最后快照：DrawIndexedPrimitive 24648 次、SetFVF 24648 次、SetTransform 108618 次；
  CreateTexture 1978 次，最大请求 mip levels=10。
- VS 配置 3 个、FS 配置 1 个；两类 key 均无容量丢失。
- draw/texture 只保留前 32 条明细，其余分别 24616/1946 条计入 dropped。
  聚合计数保留，不把这些有限样本称为全量场景。
- 所有 draw 的 bound shader pair 均为空；这不是纯固定功能 GPU 覆盖认证。
- observer 一直未连上 control pipe；原错误未保留 Win32 错误码，是本批测试工具的不足。
  模块核验原本排在 ping 后，导致 IPC 失败连带丢掉独立模块/内存观察；后续单独修正工具。
- 没有 ready、模块路径或截图见证。最大单 draw 22 个 primitive 的形态不足以证明进入地图，
  不能把这轮升级为真实地图场景证据。
- 120 秒截止后精确 Job 收口，exitCode=2、timedOut/forcedTermination=true，Job 已空。
  观察线程已结算、受保护文件未变；这是强制清场，不是正常退出通过。

## 第一轮真实输入的两宽度编译

`AutoTest/artifacts/native-ff-key-equivalence-20261023-r1/receipt.json`：ok=true。

- 使用上述真实游戏进程日志（地图阶段未证明），3 VS + 1 FS 配置，两端 4 份 SPIR-V
  大小/字节/SHA-256 完全一致；另核对 SPIR-V magic、对齐、原 key 的独立 SHA1/debugName。
- 实际 x86 生产 census 组件 422 项检查、0 失败、6064 B、allocationWindow=0。
- 两端共 28 个实际可执行文件负例，覆盖格式、重复 key、计数/数值越界及已有输出拒绝。
  31 次 native 子进程均按预期退出，没有超时或强杀，输入 Desktop 未变。
- 负例按预期 exit=1，因此原始 `desktopSafety.ok=false` 保留；独立的预期退出/清理谓词通过，
  没有把原始红值改绿。输入前后身份一致。

这只证明真实观测配置经共用编译器在两种位宽输出一致；游戏可能使用 FF ubershader，
本门不证明被编译的每个 specialized shader 都在该轮 GPU 上运行，更不是远端游戏画面对照。

## 仍开放的门

- 游戏 ready/实际地图画面、完整资源/状态覆盖、x64 执行游戏负载尚未证明。
- 产品异步 owner、idle host/window fault 通知、取消排空、真实 surface 重建、GPU 在途故障
  与完整 C2 组合仍开放，未被本次需求调查替代。
- 没有游戏内存减量、前台性能、长期压力、换图或玩家视觉等价验收。
- 保留原 `Game.dll` 代理保护，不把缺失接口静默成功、提高内存预算或漏画当作推进。

玩家 DLL 保持 36618365 B / `1FFE0BD14C51C8E1D2A9EB94425A06260CD4B480A7CA735E13E946EA93EE35CF`。
后续调查、对应原始回执与下一批最小工包在本报告追加，不追认第一轮的未覆盖门。

## 第二轮调查：确认模块，异常退出单列失败

工具补强后在 fresh `native-game-census-20261023-r2` 保持同一 DLL、地图、relative 参数、
120 秒上限、restricted-Medium/非输入 Desktop。没有改生产程序或扩大安全权限。

- 原生 Win32 错误码立即冻结；仅 2/231 可在原截止内观察启动，其余停止 IPC 尝试。
  模块核验和至多 64 次、间隔至少 2 秒的进程内存采样不再依赖 ping。
- 三个实际加载模块的精确沙盒路径/文件身份均通过，确认本轮候选确实加载。
- PID 54176；15 次 pipe open 失败均为 `ERROR_FILE_NOT_FOUND=2`，未得到 ready 或截图。
  只表示访问当时目标不存在，不证明为什么没有创建或曾经是否存在。
- 8 个进程内存样本，最后 private=1357651968 B。没有远端对照，且没有进图确认，
  不能拿它当完整地图稳态基线或迁移节省。
- 实际 `exitCode=3221225477 (0xC0000005)`，timedOut=false、forcedTermination=false，
  processSettled/jobEmpty=true；这是异常退出，不是正常退出通过。初查的产物索引漏了
  `WarVK/Crash`，曾误以为没有该轮 dump；后续独审发现真实 fatal JSON/dmp，纠正见下文。
  不能仅据退出码归因为内存不足、GPU、锁或共享 FF 代码。
- observer 已结束、cleanupErrors 空、protectedUnchanged=true；失败现场保留。
- 新工具纯测试 27/27；六项合计定向测试 fresh r2 为 **112/112**，不追认原 r1 的运行时缺口。

后续游戏试跑先停在原因分离：用未修改的玩家稳定 DLL **只读复制到另一个新隔离实例**，
保持同启动配方做控制观察。没有证据前不调整 ACL/IL、放宽验收或直接解除代理保护门。

## 同配方控制与独立源码审查

两份独立 fresh 实例沿用同一个 LostTemple、relative loadfile、restricted-Medium 非输入
Desktop 和 120 秒上限。源玩家 DLL 只读复制，没有在玩家目录运行或写入。

| 控制 | DLL SHA-256 | 实际地图见证 | 退出边界 |
|---|---|---|---|
| 玩家稳定 DLL 的隔离副本 | `1FFE0BD14C51C8E1D2A9EB94425A06260CD4B480A7CA735E13E946EA93EE35CF` | ready、模块身份、内部 framebuffer BMP 均通过，1902×963 | 截止后 Job 强制收口，未证明正常退出 |
| 本批改动前的 LLVM 候选 | `6805CC0B07DF1D1F3314ACCC5A18CF5342823D705E02FCA367F16ED3D99DF3DE` | ready、模块身份、内部 framebuffer BMP 均通过，1902×963 | 截止后 Job 强制收口，未证明正常退出 |

对应回执分别为 `native-game-census-20261023-stable-control-r1/receipt.json` 和
`native-game-census-20261023-prebatch-control-r1/receipt.json`，均在 `AutoTest/artifacts/` 下。
稳定控制 BMP 已做一次视觉检查：真实主基地、农民、树木/金矿与 UI 可见，存在投射阴影。
它是 **32 位本地画面**，不是 x64 游戏画面；本批前候选只验证 BMP 文件，尚未视觉审查。
这两份 DLL 没有新 census，runner 的 `ok=false/NOT_COVERED` 保留；分项进图成功不改写聚合结果。

本批前候选最后一次 private 为 1,355,075,584 B，与新诊断候选同量级。
因此不能从 r2 的约 1.36 GB 单点就认定“新采样器导致内存耗尽”。这些样本不是稳态或迁移 A/B。

独立只读审查对比两个真实构建 manifest：旧 1508 项均在新 manifest 中，1500 项相同，
8 项修改。除了本批 FF/census/build 接线，还有跨批的 `vulkan_loader.h`：
`vkGetPhysicalDeviceMultisamplePropertiesEXT` 从 DeviceFn 移到 InstanceFn；全 src 未见调用点。
故旧候选→新候选不是严格单变量 census 实验。

诊断候选实际 behaviorFlags=66（0x42），未请求 `D3DCREATE_MULTITHREADED`。
新诊断配置却强制启用设备锁，改变了所有既有 `LockDevice()` 的行为，不仅是统计开销。
锁为 RecursiveSpinlock，支持同线程嵌套；尚未找到成立的跨线程等待闭环，不能将访问违规
直接认定为死锁。共用 FF 的 options/key 按值复制、Rc shader 所有权与同步 wrapper
未发现新悬空引用；“未找到”不等于排除。

下一因果分离门是在同一当前源码上 fresh 构建 **census-off、保留共用 FF 与 loader 改动**，
再以同配方观察。它只能帮助缩小范围，不能单次成功就宣布锁或采样已修复。

### 控制构建与崩溃证据漏收修正

`native-game-census-off-build-20261023-r1/receipt.json` 实际 fresh x86 构建/no-work 通过，
1510 项源输入构建前后相同，受保护 DLL 保全。census=false，DLL 为
34,500,608 B / `35E95FA8F41CEE2FEF0F86A5363A984879704C70758E0BF8F2F4A1DBAD7530BB`。
源码仍含共用 FF 和 loader 修改；只是诊断编译门关闭。对应新隔离观察结果另记，不提前签收。

独审发现 r2 的 `WarVK/Crash/war3_crash_2026_10_23_15_04_08_884_pid54176_tid55212.json`
及 81,247,101 B dmp。JSON 的实例路径、PID 54176、TID 55212 和时间匹配该轮：
fatal、非 first chance，`0xC0000005` 写访问违规，指令在 `Game.dll+0x12D96D`，
写目标 `0x7FFEFF6F`。这只能定位**故障指令**，不是定位数据最初被破坏的原因。
离线 dump 复核单独保存，不修改原 r2 receipt，也不把独审 JSON 声明直接当成已展开的调用栈。

runner 已补采 `WarVK/Crash` 并抽出同一目录枚举函数，新增真实临时文件正/空例，
当前工具纯测试 28/28。原 r2 的空 `instanceArtifacts` 是工具覆盖缺口，不能用来证明没有崩溃。

### census-off 两次控制：仍未确认进图

同一 `35E95FA8…7530BB` DLL，两份 fresh 实例/原 120 秒期限：

| 回执（AutoTest/artifacts 下） | PID | 模块核验 | ready/内部截图 | 退出 |
|---|---:|---|---|---|
| `native-game-census-off-control-20261023-r1/receipt.json` | 49092 | 通过 | 均未取得 | 超时，Job 强制收口 |
| `native-game-census-off-control-20261023-r2/receipt.json` | 55328 | 通过 | 均未取得 | 超时，Job 强制收口 |

两轮均 exit=2、jobEmpty=true、protectedUnchanged=true、cleanupErrors=[]。
各有 60 个内存样本，末 private 分别 1,322,504,192 / 1,330,712,576 B；只保留观察，不当作收益。
补充后的实例索引仅发现 crash_handler_status.json，没有这两轮的新 fatal；也没有运行时等待栈，
因此不能将超时改称“同一访问违规”或“已经证明死锁”。更不能把 census/强制锁当作唯一根因。

本批后工具定向总门 fresh r3 为 **113/113**：
`native-game-input-python-20261023-r3/receipt.json`，sourcesUnchanged=true；仍非全库回归。

### 离线故障链：明确解码错误，分配来源仍未闭合

新增独立分析目录 `AutoTest/artifacts/native-game-census-crash-analysis-20261023-r1`，
不改旧 r2 回执或原 dump。匹配原始 PE 的离线反汇编确认故障为 `mov [ecx],esi`；
Game 故障函数完整 121 B 在 dump/磁盘一致。保留的 EBP 链中有
`d3d9.dll+0x812490`，该地址是 `Hook_ExecuteJassFunction` 内原函数透传 call 的真实返回边界；
它不是 census 的直接栈帧，也不单独证明 Hook ABI 正确或错误。EBP 链不是完整符号 unwind。
详细身份与证据局限见该目录 `READ_ONLY_RECEIPT.md` 和 `r2-exception-analysis.json`；
原 dump SHA-256 为 `B0E4AE3A807CC3727B87373DD153550604200500D231FEB8A88B2B6793ECAFB7`。
主线程另以匹配 PE 重跑地址映射/反汇编，并最终重算控制构建全部 1510 项源输入，0 漂移。

原实例 EXE 的 LAA 位实际为 true，未修改。Game 代码用符号选择链字段的两种解码：
正值走相对形式，非正值走按位取反；当前字段 `0x80010090` 走后者，恰好生成
故障写地址 `0x7FFEFF6F`。同一 Game SHA 的既有 StormBreaker 逆向文档也已记录这类
TSHashTable/侵入式链表双编码及高地址风险。

但 dump 没有 MemoryInfoList，相关高地址页未被保留；这**既不证明页未提交，也不证明
该字段最初是有效高地址指针**。尚缺分配源、大小/范围、写入者与寿命证据，不能宣布 LAA、
Storm、小块 allocator、内存不足、共用 FF 或采样器是已证根因。产品 TLSF 既有低 2 GiB
范围合同不能证明此次节点来自它；dirty StormBreaker 子项目仍未进入本批产品构建。

独立 FF 源码审查未找到新生命周期反例：新 helper 提前销毁 compiler，但返回 shader 的
binding/code/debugName 已按值拥有；设备/options 初始化、CS capture、cache 行为未见功能差异。
这只是源码层的有限排除，不替代当前失败的游戏门。

本轮收口保持所有游戏进程退出、玩家 DLL 不变，不发布诊断候选。继续顺序：

1. 用有界、具分配身份的证据闭合上述节点/链字段来源，不批量修改有符号跳转，不改变 LAA。
2. 诊断采集修为局部 POD/同组准入、独立有界同步，不为了计数强制整个 D3D9 device 进入 MT；
   未知状态字段保持 missing，旧 v1 证据保留，新 schema 单独严格校验。这是已审设计，尚未实施。
3. 先取得可重复的真实地图阶段输入，随后落实下列 Factory、FF state、纹理工包；
   原 Game.dll 代理保护门、产品 owner/退出/故障门继续保留。

## 下一批实际游戏接入工包（只读接口审计所得）

### A：Factory 的设备创建前协商

当前 `tools/render_host/d3d9_proxy.cpp` 的 Caps 是 64×64、单纹理/stream 的 DEV profile，
不是物理 GPU 完整能力；`d3d9_unsupported.h` 多个 adapter/Check* 仍未实现。
Connection 到 Device 构造才创建，host 在 Initialize 前没有 capability query 阶段。
因此先采集真实 Factory 查询及 CreateDevice 参数，再建立前设备协商合同。

公开能力必须是“后端真实能力 ∩ 代理已实现语义 ∩ 协议可表达能力”，复用生产
`d3d9_adapter.cpp`/`d3d9_interface.cpp` 的规则，不把缺失查询改为无条件成功。
验收包括真实查询序列的 HRESULT/输出对拍、非法参数及宿主故障后的明确失败。

### B：固定功能状态与第一类真实 draw

`SetFVF/SetTransform/SetTextureStageState` 尚未实现；proxy 声明只支持固定 stride40 实验格式，
draw wire 仍要求 VB/IB/texture/VS/PS/declaration 六个非零引用，不能直接表达 FF draw。
host DrawState 缺少 transform/material/light/texture-stage/FF 常量。

共用生产 FVF→声明、BuildFFKey 和常量准备、PositionT/缺省属性规则；不能在代理重写近似算法。
先用采集确认的一类 draw 建立状态、几何、纹理、常量的完整切片，原生32与远端64逐像素比较。
本批原始 key words 不直接晋升为跨版本协议。

### C：纹理子资源及有界上传

当前只支持 A8R8G8B8、MANAGED、单 mip、≤64×64，整层放一个 payload。
`MaxPayload=65408`，而 128×128×4 已是 65536 B，所以只调大尺寸 guard 一定不够。
需要显式尺寸/format/mips/usage/pool、分块与行 pitch、内容 revision 和完整提交后才可采样的合同。
先覆盖真实观测格式族，并测 >64KiB、完整 mip、局部更新、缺块、陈旧 generation 与每层采样。

尤其不能漏记现有 x86 Texture 的整幅 `std::vector` backing：搬走 GPU 对象不等于消掉这份内存。
须分别核算 x86 LockRect/managed backing、x64 CPU backing、GPU 对象。

### D：隔离游戏入口与组合验收

A 先行，B/C 可在文件所有权明确后并行；闭合后再引入独立 game-entry profile。
保留此前 idle fault/owner/退出工作，依次完成真实进图、同图同视角对照、正常退出与故障、
Reset/换图、x86 private/VA 与 x64/GPU 成本。不能仅解除 Game.dll guard 开始试错。
