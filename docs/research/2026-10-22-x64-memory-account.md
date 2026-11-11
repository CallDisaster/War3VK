# x86 → x64 内存账户 M0（开发实验，不是游戏减内存验收）

## 本批职责与合同

真正的x86 D3D9 COM代理通过现有C2命令owner调用原x64 DXSO/DXVK。新增冷路径显式诊断入口，
不在每个draw自动扫描，不增加GPU wait/flush，不更改预算或准入。使用固定512B小端载荷，
独立绑定真实peer PID/creation、nonce与当前sequence；解析器逐位解码并和host原始镜像对照。

分别记录：

- 公开引用为零但仍由device私有绑定的资源，仍计入实际拥有者；
- x86 buffer/texture/shader/声明/system-surface的真实CPU payload size/capacity；
- x64 owner显式持有的index/shader常量等CPU payload，另列逻辑GPU资源字节；
- 两个进程自身的private、working set、VA和peak commit；x86-before/host/x86-after是时间窗口，
  不是同时全局快照；
- 固定Connection/packet/worker存储的包含关系。packet是Connection内的一部分，不能重复相加。

不度量物理显存驻留、编译器/驱动内部CPU分配；逻辑GPU字节不能当物理显存，也不能两进程相加。
OS变化只报告范围/差值，不用随意阈值认证“没有泄漏”或“已经节省内存”。

## r1 实际隔离桌面回执

`AutoTest/artifacts/x64-memory-20261022-r1/receipt.json`：总`ok=true`，source/reference/player保全。
先运行x86/x64无GPU桌面探针，再在同一非输入Desktop执行真实96轮COM quad、256次Query；
319488像素零错，GPU submitted/completed/retired各673，577逻辑句柄创建/销毁，最终零存活。
8个采样窗口涵盖ready、上传、public-zero/private-bound、1/16/48/95轮drained及96轮最终绑定。
30条native codec/account断言通过；独立读方针对真实回执1487条变异全部拒绝。

测量是此微型负载的观察，不是游戏迁移收益：

| 账户 | r1 观察 |
|---|---|
| x86 private峰值采样 | 2,322,432 B |
| x86 used VA最大采样 | 76,238,848 B |
| x86 1/16/48/95轮drained private | 均2,322,432 B |
| x64 private采样范围 | 180,703,232–195,674,112 B |
| x64 1→95轮drained private差值 | +13,484,032 B，来源未分解，不能称完整内存平台稳定 |
| x86 Connection inline | 393,792 B，其中packet payload393,216 B；另列worker局部payload65,536 B |

x86在此期间没有加载Vulkan，实际GPU工作由x64执行；仍保留原buffer/texture/shader CPU backing。
没有同输入原生x86路径的配对内存基线，也没有Warcraft负载，因此`memorySavingsProven=false`。

## 独审发现与下一门

r1后审查发现验证盲点：host账户仅枚举live resource，若销毁路径未来遗留dead-slot vector容量，
可能被过滤成“全零”。当前destroy确实用空slot替换，未观察到此泄漏；但认证条件不应依赖这一点。
现已显式检查dead-slot实际size/capacity并拒绝残留，生产共用账户测试从30增至54条。

## r2：死槽位账户补齐后的实际复跑

`AutoTest/artifacts/x64-memory-20261022-r2/receipt.json` 总`ok=true`；54条native断言、
96轮资源复用、256次Query、319488像素零错，8窗口与1487/1487实际回执变异通过。
完整资源/句柄清零、正常host退出109ms；source/reference/player保全。此轮仍用既有进程令牌，
没有声称隐式层过滤或VVL已生效。

- x86 private最大采样2,318,336B，四个drained点均相同；usedVA最大76,242,944B。
- x64 private范围180,637,696–194,949,120B；drained 1→95增长12,804,096B，来源仍未分解。
- proxy：975872B / `2E0DFC2C3E9DD67140311D86695B47D963E66E38D8A17A81809AD3B8BF1F5B41`。
- host：5488640B / `55EFF3542D5928E6D44D132CF7528383876D5666151DC05565352EF0FEC212B4`。

真实destroy变异门只复制backend TU并替换一个链接对象；8个薄归档的170个外部对象逐个冻结，
不能只靠`.a`自身哈希。正常exit1反例必须保留原始Desktop aggregate false，清理另行严格验收。
第一轮`x64-memory-destroy-mutation-20261022-r1`在CreateDevice响应超时，尚未进入预期的
dead-capacity反例；因此失败保留，不能叫变异KILLED或重试后追认。外层Job为空、Desktop已关闭。

第二轮`x64-memory-destroy-mutation-20261022-r2`真正命中并通过负例验收：前三个checkpoint通过，
第四个phase4/ordinal1/sequence24的host镜像保留dead槽size0/capacity12、`cpu.valid=false`。
fixture只以`owned CPU account valid`退出1；child资源/句柄清零并自然退出0（125ms）。
原始Desktop aggregate和negative launch仍为false，独立cleanup接受=true，未改写raw证据。
同源原host、原object、170个薄归档外部对象、玩家DLL保全。此证据证明反例能被账户抓住，
不修复/解释第一轮初始化超时；源变动后不把这两轮拼成新候选全绿。

此账户不覆盖Presenter的5秒退出红门；源变动后需要新的完整Presenter/R1与独立VVL门。
产品异步/背压、完整D3D9/WarVK pass、游戏内存与画面A/B仍未实现，不允许据此部署或宣称完整迁移。

## r3：共享 shader cache 后的真实 Medium 账户

`AutoTest/artifacts/x64-memory-20261022-r3/receipt.json`：总 `ok=true`，restricted-Medium、
真正非输入 Desktop；原 96 轮/256 Query/319488 像素负载及 1487 条实际读方反例通过。
资源/逻辑句柄清零、child 自然退出 62ms，源码、参考和玩家文件保全。
probe、四组 CPU step、case 的原始文件在 launch-return 冻结，结束再次核验；历史无此清单的回执不追认。

- x86 四个 drained 点的 private 均 2,342,912 B，used VA 均 76,431,360 B。
- x64 四个 drained 点的 private 范围 210,788,352–214,347,776 B，1→95 轮差值 **-2,248,704 B**。
  同期 used VA 仍增加 33,554,432 B；VA reservation 不等于 private commit，也不等于物理显存。
  因此不能笼统写“完整进程平台稳定/没有泄漏”。
- 同目录 `cache-analysis.json` 以真实身份和序号绑定8个观察：192次CREATE、2次编译/注册、190 hits，
  canonical cookies/400B token storage稳定。这证明固定重复内容不再每次新编译，不证明独特shader工作集有界。
- r2/r3不是同配置、同安全身份、重复ABBA；不能将两轮OS差值相减当作修复节省量，更不能推到游戏。

生产共用实现与旧/新x86、x64三方实际像素对照见
[DXSO cache验收](2026-10-22-dxso-canonical-module-cache.md)。Presenter退出和独立VVL保持单列。
