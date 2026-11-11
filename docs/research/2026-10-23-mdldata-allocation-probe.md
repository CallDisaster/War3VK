# MDLDATAHash 分配与链边界探针（2026-10-23）

## 问题与边界

承接 `2026-10-23-high-address-link-allocation-followup.md`：旧 dump 的低节点和两个
高位链字段与 MDLDATAHash 前插规则吻合，但没有实际高地址分配历史，不能排除
损坏、地址复用或 UAF。本批补运行时观察，不改变 Game 的 signed/complement
解码、Storm/TLSF 分配策略、LAA、Shadow Arena 预算或渲染准入。

**本批最新结果：已真实记录 9 次高地址 MDLDATA 分配返回、对应入链边界，以及低节点
unlink 前携带同一高地址 backlink。** 这比旧 dump 数值重建前进一步，但没有记录实际
store PC/地址或高节点的原生 arena 路由，不能称完整根因认证或修复。探针 OFF 的独立
对照再次在同一 Game 指令自然异常；ON 两轮则均超时，三轮都未取得控制管道 ready。

原有 Storm 导出 Hook 的安装时机不一定覆盖启动期。本批在既有 bootstrap 中、
NO_GAME_HOOKS/MINIMAL 早退之后且 Storm/Shadow 初始化之前安装独立诊断。
不提前安装产品分配器。仅独立开发构建 `warvk_mdl_allocation_trace_dev=true`，
再显式设置 `DXVK_WAR3_MDLDATA_TRACE=1` 和一个全新输出路径才启用；默认编译关闭。

## 生产边界

| 入口 | RVA | 实际观察 |
| --- | --- | --- |
| Game 的 Storm401 import thunk | 120628 | 精确 source 指针的请求参数和真实返回地址；构造前不读节点 |
| Game 的 Storm403 import thunk | 1205CE | 被跟踪地址或精确 source 的 Free 调用开始/返回；返回值原样记录 |
| Game 的 Storm405 import thunk | 12062E | Realloc 开始/返回、旧/新观察序号；失败/原地/迁移分列 |
| MDLDATA 构造与 bucket 插入 | 128EC0 | 返回后的 node+0/4/8/C/10、表与 bucket anchor 的限定读 |
| global 链插入 | 129240 | 精确 table 的入口/返回边界、原 head link 与返回 node |
| generic unlink | 12D910 | 仅 ECX 等于目标表的 caller；原函数前后、caller 析构/free 之前读 |

所有原函数参数、返回值、LastError 保持透传；诊断 cell 不跨原函数调用。
只以 ReadProcessMemory 读固定大小块，失败不泄漏半块值，不追链、不解引用名字字符串。
这些是函数边界观测，**不是每条写指令的原子跟踪**。各块读也不是同一时刻的原子快照。

Game SHA `E04D1716603C075EB0C8E1E21CF1093A664ADC5249EFAB396BFA08D7B09D0C3A`，
Storm SHA `F8F519CFAA6275A5172A014F0ABED2212284390A33F1194677155A7D408E63EB`。
安装核对两文件 SHA/PE、实载基址重定位的 FF25/IAT 与函数完整前导指令；失败保留状态，
不接受 forceHooks 代替精确版本门。只启停自己六个目标，失败不销毁可能仍在执行的 trampoline。

## 所有者与成本合同

- 一份进程寿命的 Core 拥有 4096 项定长目录（最多16探测、墓碑删除）；
  allocationSerial + operationToken 约束 Free/Realloc 完成，A 的晚完成不能更改同地址 B。
- 每次目录/记录操作只有一次 atomic_flag 尝试。忙则立即拒绝，无自旋等待；
  捕获内不分配堆、不格式化、不调用 Logger/文件写入、不执行原 allocator。
- 固定映射 `256 + 16384 × 128 = 2,097,408 B`，使用 CREATE_NEW，绝不覆盖原始证据。
  映射、目录和钩子保持到进程退出；不在 live callback 中卸载。映射会带来 CPU/VA/文件页成本，
  不是“诊断零成本”。初始文件创建、页初始化及 MinHook 安装也有冷成本。
- 每条先写事实、最后发布 commitSequence，再发布已提交数；读方仅在准确所属进程/Job
  已结束后读取。强制退出可能留下半条/头尾不一致，必须标记 incomplete；不声称断电耐久。
- busy/full/探测容量/未知寿命/陈旧完成/重叠/读失败/溢出独立可见；不能相加为总丢失。
  普通事件没有额外“强制保留高地址”旁路，满日志后的证据确实缺失。

## 不允许认证的部分

1. Game thunk 之外的直接 IAT 调用、其他模块、动态导出调用、安装前分配和 Storm 内部 backing
   不在覆盖域。`GameImportForwarded` 不等于 native-small arena/TLSF/protect-memory route。
2. `+122FE0` 析构本身还会解除 global/bucket 链；未挂该路径，ECX 非目标表的 generic unlink
   也不纳入。没有记录 free 不能证明节点持续存活，目录状态只是诊断追踪状态。
3. 回调记录顺序不是所有原生内存操作的全序。边界事件 operationToken 为0，不能伪称
   已认证每次入口与返回配对；函数返回观察不等于写点原子证明。
4. 拒绝、未知、丢失或截断不能用零/默认值伪装有效证据。Python 的 certified、
   causeCertified、lifetimeCertified、allocatorRouteCertified 固定 false。
5. 本批不是 x64 游戏渲染迁移成功、内存节省、正常退出、地图 ready、玩家视觉或发布验收。

## 验证与产物

实现使用同一生产 Core/ABI adapters，CPU fixture 检查相邻六字节 FF25 的真实 MinHook
跳板和精确参数、ESP、返回值、LastError；不加载/补丁真实 Game/Storm。
Core fixture 覆盖 ABA、墓碑、容量、溢出、并发、零堆分配并输出二进制给生产读方。
静态接线门只是补充，不代替上述实际代码执行。

### 构建和生产组件门

- 独立 build：`build32_device_owner_mdl_trace_20261023`，BelowNormal、Ninja `-j2`。
  `mdl-trace-build-20261023-r1` 因 LLVM `_ReturnAddress` 未解析而链接失败，原回执保留；
  adapter 改为在 Hook 本体内展开 MSVC intrinsic / Clang builtin 后，r2 build/no-work 通过。
- 当前 DLL **34,582,528 B**，SHA-256
  `37BB9CDA329B351C502747A9EC50D8C15CF129AA19151426BC4C420376CD0F0B`。
- `mdl-trace-cpu-20261023-r2`：生产 Core **40,293 检查通过**，ABI **548 检查/0失败**；
  后者含两条相邻六字节 FF25 的真实 MinHook/跳板，仅 synthetic 内存，不执行 Game。
  Python reader **62/62**、接线 **6/6**、census reader **14/14**、census接线 **44/44**、
  survey工具 **31/31**；62项包含实际 C++ 8事件 fixture 回读，未跳过该用例。
  不是全库/Meson全配置/GPU/玩家门。
- Core 固定 **81,936 B**、映射 **2,097,408 B**；1000轮窗口 **0次堆分配**。
  没有游戏 p95/p99 或开启成本验收，不能把此处零分配等同零CPU/页/映射成本。
- 关闭编译宏独立编译同一个生产 TU：`.text/.data/.bss` 都为0，无 probe 函数或初始化符号；
  156B为 `.debug$S`，不是运行时表。产物 `mdl-trace-compile-off-20261023-r1/disabled.obj`，
  SHA `4C35094861EC312A975761FE20394BF92E4F9614C0DDCFBF69B8632288564931`。
  这只是该TU的生成代码检查，不是另一个正式DLL全量构建。
- 收口重算 CPU门记录的 **1524项源码身份，变化0**；候选和玩家DLL哈希亦未变。

### 已撤销的误通过口径

CPU r1 的读方把 `event[15]=node+4` 误称 global backlink；实际上它是 bucket next。
主线程对照 `+12D951: lea edx,[node+C]`、`+12D95B: mov ecx,[edx+4]` 后修为
`event[18]=node+0x10`，增加 w15/w16/w17-only 高位反例。r1旧结果保留，相关语义验收
撤销；当前以 CPU r2为准。第一轮游戏开始时已载入旧reader，因此不改写其receipt，另存
`mdl-trace-game-20261023-r1/analysis-corrected.json`。游戏r2由修正reader读取。

## 三轮真实游戏观察（同一候选，非输入桌面）

所有运行复用限定的 restricted-Medium 隔离桌面、独立实例、同一LostTemple地图和120秒上限。
不是前台性能AB，也不是地图等价验收；不修改分辨率或系统设置。

| 运行 | PID / creation100ns | 原始终态 | 实际证据 |
| --- | --- | --- | --- |
| `mdl-trace-game-20261023-r1` ON | 51564 / 134372285105255137 | 120秒超时、Job强制结束、exit2 | 64事件、8次低地址分配；最后导出Present1024/draw24648 |
| `mdl-trace-game-off-20261023-r1` OFF | 55184 / 134372289235518557 | **自然0xC0000005**，不是超时/强杀 | 无trace文件；同Game+12D96D写异常及81,320,445B dump |
| `mdl-trace-game-20261023-r2` ON | 56416 / 134372291507422490 | 120秒超时、Job强制结束、exit2 | 112事件、19次分配，其中9次高地址；最后导出Present16384/draw393288 |

三轮都通过实际 Game/Storm/DLL 模块身份核验。ON日志各自无半发布尾部、九项诊断计数0、
无未完成的Free/Realloc token；只说明本有界日志内部一致，不覆盖未挂钩路径或全部寿命。
最后导出的 Present/draw 是阶段性累计见证，不是退出时最终计数或地图阶段证明。

每轮 `ready=false`、无截图；管道请求都在 `CreateFileW` 得到 Win32=2，没有成功ping，
**尚未执行地图ready谓词**。不得把“未连上管道”写成“已经进图但地图ready不满足”。
三轮 Job均空、Desktop均关闭、输入仍Default，cleanup errors为空；游戏接受/正常退出仍红。

### ON r1：排除合法哨兵误报

8次分配全部为低地址，request22404、source `Game+B7FF90`、caller-return `+128EE2`。
两条unlink前高位字段均为 `0xA291952F = ~(GameBase+BC6AC8+8)`。
表/入链边界及原初始化/解码代码共同支持它是本表合法锚点补码，不是高地址节点。

### ON r2：真实高地址返回与低节点backlink关联

19次分配均为22404B、flags8、同一source/caller；9次返回落在 `0x80A30090..0x80C10740`。
每个高返回均有同pointer/serial的 BucketReturn与InsertEnd。最强的同进程链如下：

| event sequence | 真实记录 |
| --- | --- |
| 70 | Alloc低节点 `0x7FC45820`，serial10 |
| 74 | Alloc高节点 `0x80A30090`，serial11 |
| 75/76 | 同serial11 bucket/global返回；old-head `0x7FC4582C` 的 `+4`已为 `0x80A30090` |
| 78/80 | 第二高节点 `0x80A35820` 分配并入链 |
| 81 | 低node serial10的 `node+0x10=0x80A30090`，不是锚点补码，而是先前实际返回值 |
| 82/84 | 低node的UnlinkEnd及FreeEnd确实返回；四link字段归零，free原始返回1 |

若冻结的原consumer实际消费seq81这个值，有符号分支将计算
`~0x80A30090=0x7F5CFF6F`，而非前驱link位置 `0x80A3009C`。
**这是条件指令复算，不是实际store事件**；错误写落在可写位置也可能不立即异常。
当前没有对应PC/store地址/页属性或unlink后前驱next读数，不能认定已证明这次写污染，
同样不能把返回解释为“高地址链安全”。9个高node均没有被记录到unlink/free，不能认证其仍存活。

### OFF：第二个模型复现相同异常，新增低arena快照

新失败node `0x7FFA5820` 名称为 `buildings\undead\AltarOfDarkness\AltarOfDarkness`，
不是旧UBirth。consumer完整121字节与精确磁盘版本一致；故障backlink为 `0x80C50090`，
取反恰为异常写目标 `0x7F3AFF6F`。首14项EBP返回与旧故障一致。

这次保留了低arena `0x7FFA0000`：source `.?AUMDLDATAHash@@`、sourceLine=-2，
两低块header为 `00045790/6F6D7FFA`，live2/requested44808B、commit45056B/reserve65536B，
external字段0。它是本次两个**低地址原生小块**的来源快照，不是高节点的分配史。
高节点和高arena页仍未保留，无MemoryInfoList；不能将缺页读成当时未映射。
四候选地址前插与六个保留字段吻合仍属数值模型，不与ON r2拼接成同一生命周期。

## 收口与下一步

1. **本轮闭合**：有界生产观察器、实际ABI与writer/reader往返、高地址真实返回和入链/消费前字段关联。
   **尚未闭合**：实际错误store、完整高块寿命/arena route、无ready原因、正常退出、修复/性能/视觉接受。
2. 下一次运行先补初始化阶段见证，不无差别重复120秒：现有 `war3dbg::Print` 写
   OutputDebugString，不写 `War3_d3d9.log`。没有这些日志不能证明Storm/生命周期未装。
   当前bootstrap实际上会提前尝试Storm安装；先安全复用唯一DBWIN listener的启动游标+精确PID，
   若不可复用，再设计独立默认关闭定长stage POD。不得开第二DBWIN listener或重启共享服务。
3. 修复设计优先考察**此模型分配域的低地址兼容约束**与现有native-small/protect-memory/free/realloc
   的衔接，不把22KiB请求强行改成全局TLSF大块、不泛改 `jg/not`、不调高Shadow Arena或改LAA。
   必须有低地址正常行为、合法哨兵、失败/回收/地址复用及完整模型覆盖的生产测试，再做受控游戏A/B。
4. 磁盘另有同source的 `+12B290` 分配函数，仍走被观察的Storm401 thunk，但本批未找到直接/绝对
   引用或运行时样本，不能称可达漏口；其挂链descriptor也未证明是目标表。保留覆盖未知，不盲目加Hook。
5. 未覆盖玩家DLL、未修改EXE/Game/Storm/LAA/预算，未提交/推送/发布。玩家DLL仍为
   `1FFE0BD14C51C8E1D2A9EB94425A06260CD4B480A7CA735E13E946EA93EE35CF`。
   本调查仍是64位迁移的真实游戏准入前置，不是x64整幅画面或游戏内存节省证明。

## 回执索引（均在 AutoTest/artifacts/）

- `mdl-trace-build-20261023-r2/receipt.json`、`mdl-trace-cpu-20261023-r2/receipt.json`。
- 三个上表运行目录各自的 `receipt.json`；r1修正读方另存 `analysis-corrected.json`。
- `mdl-trace-evidence-20261023-r1/summary.json`：主线程按准确退出身份重新调用生产reader的三轮汇总；
  同目录 `summarize.py` 可复算，输出CREATE_NEW；`READ_ONLY_REVIEW.md` 为ON r2独立原始字节复核。
- ON r1原bin SHA `598EFEC83B3B111D524D93C161F1C007E86C4D8642C95A48622B8421CB59466A`。
- ON r2原bin SHA `5B25EE83132F212AC806419C72A3411447DACBA0915FA0F43FF65DDAFEA597D2`。
- `mdl-trace-off-crash-20261023-r1/READ_ONLY_RECEIPT.md`、`off-exception-analysis.json`、`analyze.py`：
  锁定OFF dump SHA `EB1A46D6CFA3DEF32EC36251C362330484AEAFF55C5FBAEF82C554E1805823F4`，纯文件分析。
- `mdl-trace-compile-off-20261023-r1/`：独立编译宏关闭的符号/节检查。
