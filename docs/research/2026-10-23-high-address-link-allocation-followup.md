# 2026-10-23 高地址链字段：MDLDATAHash 分配与入链路径追踪

## 本轮结论

定位已从“某个 Game 链表读到了高位值”，收窄到 **ModelCreate 路径的
`MDLDATAHash` 容器、它的 22,404 字节分配点，以及把新节点地址写进前一节点
backlink 的具体指令**。失败时的低地址节点名称为
`buildings\Undead\Ziggurat\UBirth`。

这不是 Stage11 快照页或 WarVK Shadow Arena 的分配点。该尺寸低于产品 TLSF
接管阈值；按当前产品尺寸策略，属于 **原生透传请求**，不能用“TLSF 已限制低
2 GiB”排除它的高地址风险。尺寸路由本身不证明未保留高节点在 Storm 内部实际走
small arena 还是 protect-memory 的 external VA 分支。

不过，**高地址节点的实际分配回执和首次插入历史仍缺失**。现有 dump 不含
这些高页和 `MemoryInfoList`；以下结果是“精确版本的静态分配/写链路径 +
保留内存的相关字段 + 数值重建”，不是完整运行时来源证明，也不是修复完成。

本报告对应的分配调查只做文件级分析，未运行/附加分析目标，未修改 Game.dll、
War3.exe、LAA、Storm/产品 allocator 或系统设置。该分工只写本报告与专用分析产物；
同轮采集器实现及另行隔离游戏观察见文末 checkpoint，不将其当成分配探针实测。

## 输入身份与复算产物

| 输入 | SHA-256 |
| --- | --- |
| R2 `Game.dll` | `E04D1716603C075EB0C8E1E21CF1093A664ADC5249EFAB396BFA08D7B09D0C3A` |
| R2 `Storm.dll` | `F8F519CFAA6275A5172A014F0ABED2212284390A33F1194677155A7D408E63EB` |
| R2 dump，81,247,101 B | `B0E4AE3A807CC3727B87373DD153550604200500D231FEB8A88B2B6793ECAFB7` |

输入仍位于：

`E:/Work/War3_AutoTestSandbox/_AutoTestInstances/native-d3d9-survey-20261023-r2/survey-01/root`

先前异常/线程/模块身份的原始审查见
`AutoTest/artifacts/native-game-census-crash-analysis-20261023-r1/READ_ONLY_RECEIPT.md`。

本轮复算脚本及 JSON：

- `AutoTest/artifacts/native-high-link-allocation-20261023-r1/analyze.py`
- 同目录 `bounded-source-dump.json`
- 同目录八份 `game-rva-*.txt`：仅相关函数的有界反汇编。

脚本锁定 Game/dump SHA，直接解析 PE section/import 表，用已有 minidump
库读取已保留字节，调用已有 LLVM objdump 反汇编指定范围；不执行被分析的 DLL。
不依赖反汇编输出中远处最近导出符号 `GameMain` 的伪函数标签。

## 1. 失败对象不是从 JASS 栈名猜出来的

Game 首选基址为 `0x6F000000`，R2 实载基址为 `0x5CB20000`。下面均以 RVA
表达代码/静态对象身份。

| 证据 | 观测 |
| --- | --- |
| 失败 consumer | `Game+0x12D910`，异常写指令 `+0x12D96D` |
| caller | `+0x12833E` 的 call，返回点 `+0x128343` 与 EBP 链一致 |
| caller 使用的表 | `Game+0xBC6AC8`，R2 地址 `0x5D6E6AC8` |
| 表 vtable | dump `0x5D481788`，即 `Game+0x961788`，与匹配磁盘 vtable 一致 |
| 表全局链字段 offset | `table+4 == 0x0C` |
| 失败 node | `0x7F165820`；`node+0x14 == 0x7455A008` |
| 保留的 node 名称 | `buildings\Undead\Ziggurat\UBirth` |
| 分配来源 type-name | `Game+0xB7FF90` 的 `.?AUMDLDATAHash@@` |
| 邻近创建路径字符串 | `Game+0x961828` 的 `ModelCreate.cpp`、`+0x9617DC` 的 `HMODEL` |

`+0x128270` 在 `+0x1282FC` 调用 `+0x12B740`，以 hash/name 查表。
命中后使用 `node+0x18` 构造/转换模型，再在 `+0x12833E` unlink，随后才经
表 vtable[0] 销毁节点。vtable[0] 为 `+0x128BF0`，最终调用 Storm free wrapper。
这是模型数据 hash 容器路径的直接代码证据；不得仅因为栈更上层含 JASS forwarding
就命名为“JASS 分配器导致的损坏”。

## 2. 已找到分配点及所属策略域

vtable[1] 是 `Game+0x128EC0`：

```text
128EC3  mov eax,[ebp+10h]       ; flags
128EC9  or  eax,8              ; zero-new-memory
128ECC  push eax
128ECD  mov eax,[ebp+0Ch]      ; extra bytes
128ED0  push -2
128ED2  push 6FB7FF90h         ; .?AUMDLDATAHash@@
128ED7  add eax,5784h
128EDC  push eax
128EDD  call 120628h
128EE2  test eax,eax
128EE6  mov ecx,eax
128EE8  call 1220E0h           ; node construction
```

`+0x120628` 是 `jmp [Game+0x94E678]`。PE import 表准确把该 IAT 槽映射到
**Storm.dll ordinal 401 / SMemAlloc**，不是依据邻近地址推测。

`+0x12A930` 的具体调用点 `+0x12A98A..0x12A992` 在已证明 `eax==0` 的
分支依次压入 0、0、hash，并调用 `+0x129240`。后者 `+0x129280` 经表
vtable[1] 调用上述分配器，所以这条路径的请求为：

```text
requested = 0x5784 = 22,404 bytes
source    = .?AUMDLDATAHash@@
sourceLine/argList = -2
flags     = 0 | 0x08
```

`src/d3d9/war3/memory/war3_storm_hook.cpp:38` 定义接管阈值 `0xFE7C`；
`:747` 开始的 `Hooked_SMemAlloc` 在 `size < threshold` 时调用原生分配器。
安装只允许把阈值调高，不允许调低进入小块域。因此这条请求即使经过当前产品
Storm hook，也仍应落在 native-small，不会因现有 TLSF 大块低地址保护而自动安全。

这里的 native-small 是**产品按请求尺寸选择的原生回退域**，不是对两个未保留高节点的
Storm 内部分配分支认证。同版本 Storm 的 protect-memory 选项也可能让小请求走外部
VirtualAlloc；生产 `war3_storm_native_small_repair.cpp` 在该模式下绕过 small-search 修复。
低节点压缩头是实际原生小块的正证据，不能将它外推成高节点也必定来自同一 arena。

这仍是当前产品策略和该精确 Game 版本的静态结论：R2 dump 没有保留 allocator
IAT 的实值或 Storm 导出入口代码，不能据此声称已复原当时全部 Hook 安装状态。

### 低地址 node 的头部与原生块吻合

保留字节：

```text
[0x7F165820 - 8] = 0x10045790
[0x7F165820 - 4] = 0x6F6D7F16
```

按已经针对同 SHA Storm 核对的压缩头布局：total bytes 为 `0x5790`，padding
为 4，flags 为 `0x10`，magic 为 `0x6F6D`，arena 高 16 位为 `0x7F16`。
`align8(0x5784 + 8) == 0x5790`，与上述请求一致。

该证据支持低地址失败 node 属于这类原生 Storm 块；它不是高地址对象的 live
allocation receipt。其推导 arena `0x7F160000` 的头部本次亦未保留，无法读出
实际 logical heap ID、source-line、reserve/commit 或该 arena 的增长史。

原生 Storm 的 `StormHeap_Create @ +0x2A350` 在 `+0x2A388` 发起
`VirtualAlloc(NULL, reserve, MEM_RESERVE, PAGE_NOACCESS)`，随后 commit；该
函数只检查 NULL，没有本地 `< 0x80000000` 范围门。与之不同，产品
`war3_tlsf_pool.cpp:97` 的 `IsStormCompatibleAddressRange` 约束的是 TLSF
拥有/返回的范围，不覆盖原生 Storm 新建 arena。

## 3. 确认了实际写 backlink 的位置

`Game+0x129240` 把新节点挂入表的全局链。下面不是抽象 TSHashTable 模板猜测，
而是本表 vtable 和 `table+4==12` 对应的具体实例：

```text
129280  call [edx+4]             ; allocates/constructs node, EAX=node
129283  mov ebx,eax
129289  mov edx,[esi+4]          ; 12 for this table
12928C  add edx,ebx              ; newLink = node+12
...
1292C2  mov ecx,[esi+8]          ; oldHeadLink
1292C5  mov [edx],ecx            ; new.next = oldHeadLink
1292C8  mov eax,[ecx+4]
1292CB  mov [edx+4],eax          ; new.prev = oldHead.prev
1292D0  mov [ecx+4],ebx          ; oldHead.prev = new NODE address
1292D3  mov [esi+8],edx          ; table.head = new LINK address
```

这说明 **高位新节点地址可以在插入阶段被直接写入旧节点的 backlink**；这几条
赋值本身不按符号解码该新地址。它可以稍后在 unlink 旧节点时才触发错误，不能
假设“如果真是高地址，必定刚分配/刚插入就崩”。

当前失败端则读取 `node+0x10`：

```text
12D95B  mov ecx,[edx+4]
12D95E  test ecx,ecx
12D960  jg   relative_decode
12D962  not  ecx
12D96D  mov [ecx],esi
```

负数形态既可表示取反哨兵，也可能实际来自超过符号边界的 node 地址，因此
**不能将这一个 `jg` 机械改成 `jnz`/无符号比较**；那会破坏合法的取反哨兵。
完整 consumer 121 字节在 dump 中保留且与磁盘完全一致；其他新追溯函数不全在
dump 中，产物明确将其 `entireRangeRetained=false`，不伪称 live 字节认证。

## 4. 四字段数值重建：强一致性线索，不是历史录像

将以下三个数值依次作为新节点输入上述全局链插入规则：

```text
low   = 0x7F165820
high1 = 0x80010090
high2 = 0x80015820
```

从合法空链初始化出发，数值模型可精确生成四个已保留字段：

| 字段 | dump 实值 | 数值模型 |
| --- | --- | --- |
| anchor.next | `0x8001582C` | `high2 + 0x0C` |
| anchor.prev | `0x7F165820` | low |
| low.next | `0x5D6E6AD0` | anchor |
| low.prev | `0x80010090` | high1 |

随后 unlink low：符号选择导致 `~high1 == 0x7FFEFF6F`，与异常写地址完全相同；
若选择该节点格式的相对式，应计算为 `high1 + 0x0C == 0x8001009C`。
另外 `high2 - high1 == 0x5790`，与该原生请求的块跨度一致。

这说明“同类型 native-small 分配跨过 2 GiB，前插时保存裸节点地址，稍后删除
原低地址尾节点时误解码”是一条 **可逐字段解释 dump 的高优先级机制**。
但两个高地址节点都没有保留，构造序列是测试输入而不是观测到的事件。仍不得
排除先前非法写、UAF、地址复用或其他路径传播相同值；也不得声称两个高地址
在异常时确实 committed、可读或 live。

## 5. 下一份最小运行时证据应该采什么

先完成本轮“不强制设备多线程锁”的采集器改造与隔离稳定性门，再决定是否接入
下述独立开发探针。**本报告没有实现或启用这些 Hook。**

1. **精确分配域回执。** 在现有 SMemAlloc 的实际 route 选择与原调用返回后，用
   局部 POD 捕获 `sourceRva`、request、flags、argList、返回地址、原调用者 RVA、
   native/TLSF/fallback 路由、线程和单调 allocation serial。精确筛选
   `source == Game+0xB7FF90`；不要热路径解引用 name 字符串，也不要只筛高地址而
   丢掉前驱低节点的起源。如果 Hook 尚未安装/版本不匹配，输出 coverage-missing。
2. **同域 free/realloc 历史。** 地址要配 allocation serial/lifetime，成功/失败、
   原地/迁移分别记；不能将相同地址的两次分配合并。一个固定容量目录需要明确
   `tableFull/evicted/busy` 口径；淘汰后不得把节点说成“一直存活”。
3. **入链与删除边界。** 对精确版本的 `+0x129240` 与 `+0x12D910`，在已获授权的
   隔离诊断实例内建立限定 wrapper/断点观察，绑定 `table==Game+0xBC6AC8`。
   取实际局部 new-node、old-head、写入值和关联 allocation serial；不把分配器
   返回值等同于“这次链写已经发生”。观察 Hook 的 ABI/寄存器/stack 与安装字节
   必须独立验收，不能为获得证据去修改 `jg/not` 的业务结果。
4. **低频地址域确认。** 一旦出现本域首个高位新节点，在分配器之外的诊断安全点
   记录 `VirtualQuery` 的 AllocationBase/State/Protect/RegionSize，再记录
   完整范围；异步查询不能拿来证明过去某一瞬间状态。优先让下一份 dump 保留这
   几个精确 node/arena 头与 `MemoryInfoList`，而不是打开无界全进程追踪。
5. **探针本身的边界。** 定长表/环、总事件预算、单次 try-cell、拒绝可见；只在
   cell 内交换 POD，不持有 cell 调 allocator/游戏函数/Logger/文件 IO，不改
   分配返回值/阈值/序列。session、PID+creation time、Game/Storm/DLL SHA、
   map epoch 与时序必须成套绑定；关闭探针时不新增查询/读取/锁。

足够支持修复设计的闭合链是：

`native-small 分配成功 → 对应 allocation lifetime → 该 node 的入链实际写值
→ 对应旧 node 删除前的字段 → consumer 选择分支 → 地址域状态/异常`。

缺任意环节都应保留“来源未认证”。当前高价值方向是约束/迁移这个 native-small
模型数据域，或设计版本锁定的容器表示兼容方案；不能通过扩大 Shadow Arena、
强制所有 D3D9 调用拿设备锁，或泛改数千个有符号分支来代替它。

## 证据上限

- 已完成：精确文件/PE身份、MDLDATAHash 静态分配与入链路径、失败低节点名称和
  原生头、consumer live字节匹配、四字段数值重建。
- 未完成：高位节点/arena 实际分配历史、首次写事件、live 页状态、受控复现/修复 A/B、
  正常退出和长时/地图接受。独立 try-cell 候选已另跑两次隔离观察：有严格 v2 日志，但
  都未取得 ready/截图并在 120 秒强制清场，不为本报告补造分配历史或根因认证；见
  [采集器 checkpoint](2026-10-23-census-try-cell-checkpoint.md)。
- census-off 的 ready 超时与这次 `0xC0000005` 是不同终态。这里的发现不能证明
  强制设备锁或新 FF compiler 导致了该异常，也不能解释全部超时。
