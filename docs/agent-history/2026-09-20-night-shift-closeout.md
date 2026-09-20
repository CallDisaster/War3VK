# 2026-09-20 夜班收口：离线候选完成，阴影恢复门未过

## 一句话结论

四线夜班完成了 S2 局部成本收口、真实快照页账户、保守分页纯组件、测试运行器与读方修正，以及一个无用 UV 引用的最小寿命修复。**没有完成阴影消失修复，没有新的稳定版，不合并、不提交、不发布。**

按 08:20 停发长批、08:30 收口、09:00 停止的夜班计划执行；最终资源/自动化结算记入协调账本和开发日志。C++、Shader 与测试源码已冻结，无新的编译或游戏租约。

## 实际交付与验证层级

| 方向 | 本夜实际完成 | 已取得证据 | 未完成 / 不可宣称 |
| --- | --- | --- | --- |
| S2 palette 计算 | 调用方输出 vector 容量复用；固定 256 项去重；core 合计恢复为两趟、upper 一趟；整数半开区间 alias/溢出保护 | 300010 core + 300005 upper 输入，0 mismatch；CPU 成本夹具连续调用 pointerChanged=0 | 适配层为派生建模，不是完整 Game.dll 来源链链接测试；仍有 prefix scratch 分配，不能说零分配或游戏 FPS 已提升 |
| P1b 页账户 | 按页容量、used、cache slice capacity 并集、无 cache 引用 used、寿命标签观察 | OFF03 / ON01 真实 384 MiB 压力；页尾与稀疏长期引用数据 | 不知道全部 GPU 持有者/最后消费者；无 cache 引用不等于可安全覆盖或物理显存已可释放 |
| UV 寿命 | 成功 no-UV 捕获后、激活前解除旧独立 UV backing/proof；有效 UV 与失败捕获规则保持 | 661 CPU checks；5 静态；active-ledger 4 例及 4 变异 | 新 DLL 尚未实机；不能认为它已解决池满，未测到实际节省字节 |
| P2a 上传索引范围 | 保留默认关闭的有界候选和测试；检查真实 ON 覆盖 | 两个完成路线均 uploadRangeHits=0 | ON 本图没有命中；拒绝累计数差异不算收益；512 KiB POSITION fallback 不是 INDEX 上传大小 |
| P3 寿命分页 | 同共享预算、同类优先且保留合法混合借用的纯政策；严格模式默认 false | 16 Python、120 编译 CPU checks，独立 20000 随机模型 | **未接生产 device**；创建失败/异常安全前置缺口未修；不是显存优化已生效 |
| 剔除 | 审核 release 门、反例与各消费者条件 | 保守模型/静态和源码证据 | Consume 仍 Off；最终 CSM draw 剔除晚于快照分配，不能解决当前捕获分配压力；未知/动态 bounds 不可乱剔除 |
| 测试与报告 | 初始完整帧等待、精确进程句柄恢复；报告帧绑定；fresh high-water 与多报告并集；direct unittest 入口补齐 | 最终 270 个静态脚本、1340 个 unittest 用例；定向读方 56；Meson 91 | 用例/脚本/重复跑不相加；压力/恢复/完整玩法和视觉门仍未过 |

测试成本夹具的 reused-output 分配从旧实现 2 次/64 B 到当前 1 次/32 B（本 TU operator new 口径）；其中仍有 prefix scratch，不能把去重/输出分配消除说成所有分配消除。已有约 60% 实现 / 40% 验收的长期管理估算不因本夜增加测试数而自动上调；未重新做全项目百分比审计。M3/M4、统一选择权限、跨 Pass 消费寿命与压力恢复仍未完成。

## 阴影消失：本夜真正缩小的问题范围

视野内几何不需要达到 384 MiB，快照页池也可能已满。OFF03 的四个 16 MiB 活动页，只剩约 2.503 MiB 带静态标签的 cache slice capacity 引用，却牵住合计 64 MiB 页容量。首次容量拒绝附近的全池账户是：

- 活动页容量 384 MiB；cache 区间并集约 186.006 MiB；used 中无 cache 引用约 186.909 MiB；未分配尾部约 11.085 MiB。
- 后续 cache 并集约 172.486 MiB、无 cache 引用 used 约 200.429 MiB，活动页仍 384 MiB。
- 这支持“不同寿命混放、少量引用牵住大页”的诊断，不证明所有增长来源，也不授权页内覆写、强制清缓存或越过 GPU 最后使用。

全场景失影的放大链条已与源码/报告对齐：

`ResidentCapacity → 必需 caster 遗漏 → 整份阴影候选不完整 → clear/draw 前返回 → 最多 8 帧旧图保留 → 无完整阴影图 / 零强度`。

不能以延长旧图、减少 caster、关闭效果或提高 384 MiB 来把此门变绿。详见 [逐帧压力记录](2026-09-20-night-runtime-pressure-findings.md)。

## 实机运行没有被写成通过

本夜共有四个独立冻结的启动尝试，原始失败均保留，不被后续新运行覆盖：

| 目录（AutoTest/artifacts 下） | 结果 | 结算 |
| --- | --- | --- |
| night_stage11_census_off_20260920_01 | 完整帧初始 anchor 失败，未形成合格路线 | 已条件恢复、零本轮残留 |
| night_stage11_census_off_20260920_02 | menu-ready-not-started，未进入路线；不是压力/恢复证据 | 已条件恢复、零本轮残留 |
| night_stage11_census_off_20260920_03 | 路线完成、9 HTML / 4 BMP；压力后恢复 FAIL | 原生 hProcess 自然 exit 0，恢复成功 |
| night_stage11_census_on_20260920_01 | 路线完成、9 HTML / 4 BMP；P2a 命中 0、恢复 FAIL | 原生 hProcess 自然 exit 0，恢复成功 |

两个完整路线使用旧诊断 DLL `8B40212050A9C9CC7F0397F490C5D5F81D633F4D16F65A1DB205934DC3EE788E` / 36,399,883 B；非交互隔离桌面 2560×1440、零 global input。有已限定的单次隔离窗口消息 SPACE 就绪脉冲，并非“完全没有输入操作”；没有全局键鼠。相机移动/返回可见，但难度选择对话框仍在，因此不认完整玩法/视觉通过。

OFF 返回窗口图序号停在 3164，ON 停在 2990，均没有真实新高；末端还分别缺 106 / 177 帧报告覆盖。旧读方曾把 0 占位后的旧序号误算 fresh，仍最终判 FAIL；新读方修高水位和 strict 多报告合并，旧产物不回写，不放宽任何门。

## 构建、失败与最终纯离线验证

- validation-b19：BelowNormal / -j2，DLL preflight/actual 2/2（device obj + DLL link）；exact no-work；两个明确 CPU targets preflight/actual 4/4；CPU 661/120，Meson --no-rebuild 91/91。
- 该批静态最初 **86 PASS / 1 FAIL / 183 未跑**：旧文本门仍要求 commit 后无花括号直接 activate。这是保留的失败，不追认成成功。
- 父任务仅修测试，要求成功 commit → 仅无当前 UV 时清理 → 原完整 key 激活，4 个真实反例均被杀死；C++/DLL 不变。
- validation-b20 新跑：270/270 静态脚本、1340/1340 discovery 用例、读方 direct/module 各 56、UV 5、政策 16、ledger 4、CPU 661/120。重复执行不累计成更多独立门。
- 父任务另核对应 5 文件 py_compile、git diff --check、源冻结、包 CRC/逐条 size/SHA、PE32/i386、现场哈希及零相关进程。新候选没有运行时/GPU/视觉接受。

## 最终离线包

- DLL：36,400,299 B / `E42CD88CD266CAF950E77001386C71B3B79105DE0A8AD41272C414B87124E814`，PE32/i386。
- 包：`AutoTest/artifacts/night-shift-20260920/WarVK-night-20260920-E42CD88C-offline.zip`。
- ZIP：8,035,766 B / `9E7F29971240D3424DD488222B5ACEDF22B7220BA4473798419CF9010AE29F88`。
- 仅含 DLL、README、manifest；无自动部署/启动脚本。原字节 CreateNew 冻结并独立核验，**离线诊断候选，不是已修复的试玩/发布版**。
- 编译/测试冻结：`source-freeze-0800-python.json`，2970 项 / `BCA05FDFDAA52CC0AEE41974E3E55054E0A9A38C36FD8A7FED22CA80AAE32727`。之后只有主任务收口文档/AGENTS/总日志变化，代码/测试保持；另记收口冻结，不篡改包内旧 pin。
- 最终测试现场 `E:/Work/Warcraft III/d3d9.dll`：36,359,521 B / `BF938FBB6FBBB0B84A64EFE3A3E32545AAB98402D17180D2BDD6CA466A460CB3`。
- 保护现场 `E:/Work/War3/d3d9.dll`：36,271,456 B / `A0A51AF2BB9091B2C677DC34BEBDF76C02347049A89D0BB2AEF4C37518E52134`，未部署。

## 本夜差异而非累计脏树

02:38 备份保留 882 个原 dirty 路径。以 08:00 冻结实字节对比：原 882 中 28 个继续变化、853 个未变，另 DEVELOPMENT_CHANGELOG 为冻结排除项但实际有增量；新增 17 个文本路径。没有把此前 903 项 dirty 全算作本夜成果。

收口文档前的增量（代码/测试分别计）：

- 原 src 15 文件 +706/-197；新 src 411 行。
- 原 AutoTest 13 文件 +3240/-177；新 AutoTest 4730 行。
- 原总日志 +183 行；新 docs 1186 行。此后本收口文档和 AGENTS/总日志另算。
- 大量增量是测试/证据合同，不代表对应运行能力已成熟。完整路径见 ignored `architecture-b21/final_delta.json`；原字节基线在 `D:/WarVK-Backups/20260920-night-0238`，不删除。

**08:26 主任务独立补正**：architecture-b21 声称“没有原本干净的 tracked 文件变化”不成立。将 backup 的 882 路径与当前 `git status -z` 对比，发现另有 4 个原本干净的 tracked 文件在早期批次改变，0338 快照已含其修改，所以它们被该脚本漏算：

| 原本干净的 tracked 路径 | 新增 / 删除行（HEAD 即开工基线） |
| --- | ---: |
| AutoTest/test_union_consumer_visibility_static.py | +56 / -0 |
| src/d3d9/war3/render/tests/war3_union_consumer_visibility_test.cpp | +133 / -1 |
| src/d3d9/war3/render/war3_union_consumer_visibility.cpp | +44 / -0 |
| src/d3d9/war3/render/war3_union_consumer_visibility.h | +14 / -0 |

故收口文档前完整代码/测试口径为：原 src **18 文件 +897/-198**，原 AutoTest **14 文件 +3296/-177**，再加新 src 411 行与新 AutoTest 4730 行，合计 **+9334/-375**（含 C++ 测试，不等同生产代码净增）。17 个新文本路径之外还有这 4 个“新增 dirty、不是新增文件”的路径，解释 882→903；本收口文档新增后为 **904（110 tracked +794 untracked）**。本夜原始报告保留，以上父审补正优先，不能继续引用旧清单的排除结论。

## 新发现但故意没有仓促接入的风险

分页“新建同类页优先于借别类尾部”会新增分配尝试，当前 DXVK createBuffer 失败合同不足：底层可返回 null，而 assignStorage 解引用；构造先注册资源，随后抛出时派生析构不运行，resourceMap 可能保留悬空条目。bind 失败的 Vulkan allocation 有 Rc RAII，不能泛称所有 Vulkan 资源泄漏。这是源码风险，不是已确定玩家崩溃根因。

因此 P3 只保留纯组件，不接生产。后续先证明类型化失败/注册回滚/DeviceLost 不重试，再讨论合法尾部回退，不能 catch-all 吞错。memory-b20 的过强泄漏说法已由 b21 收窄。

## 下一次恢复工作的优先级（本夜不再启动）

1. 最小、独立解决上述分配构造失败/注册回滚合同，补生产共有的失败注入测试；不扩大到全分配器重写。
2. 为 P2a 零命中补有界调用点、索引大小、summary accepted/bypass、query miss 的少量计数。现有 late hit 一项不足以区分原因；不读 WC/GPU 内容、不靠放大预算获得覆盖。
3. 在安全分配前置通过后接 P3 受控生产适配，仍同一 384 MiB、同代/last-use、不放弃可用合法尾部；用页寿命 census 判断是否真正减少稀疏长期页。
4. 先解决自动路线的难度界面，再做同场景压力→移开→返回→持续恢复，保留有效 caster 完整性和实际图更新；必要时玩家视觉单列。
5. 通过上述门后才考虑 v1.22 合并/发布。x64产品、未完成 Water、自动原版模型灯继续延期；长期 M3/M4/跨 Pass 收口仍需后续批次。

## 协作与证据纪律

四线均沿用户指定 commandcode / deepseek-v4.1-flash。architecture/memory 末轮报告已落盘，但最终回执发生 CONTEXT_WINDOW_EXCEEDED：保存为失败 turn，父任务接管核验，没有伪称正常完成，也没有在截止前另开模型继续扩工作。validation/culling 有正常 wait_turn 完成回执。所有源写者均冻结，源码与原始坏帧未删除，无系统文件改动、提交、合并、推送或发布。

主任务验证回执：`AutoTest/artifacts/night-shift-20260920/parent-final-verification-0818.json`；报告、leases、各 turn 终态与停止状态统一见同目录 `coordination.json`。
