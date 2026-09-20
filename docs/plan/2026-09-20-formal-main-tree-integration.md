# 2026-09-20 v1.22 本地主树正式整合事务

## 授权与操作范围

用户明确要求“开始正式合并”。本次执行已审查的 B-primary 源码整合与原主目录切换，
不是将旧 A 全量 merge 覆盖 B，也不是产品发布。仅本地 Git 提交/分支/保留型 stash/switch；
不推送、不打标签、不改玩家 DLL、不构建或启动游戏。

- A：`E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk`，旧 HEAD `88089cdf90f728e85b91bf45d75002348574b665`。
- B：同级 `dxvk-v1.22-integration-20260914`，旧 HEAD `ae890542d766470d1703f5bea7f5b73636039733`。
- B 源码 checkpoint 分支：`codex/v1.22-source-checkpoint-20260920`。
- A 整合后分支：`codex/v1.22-main-tree-20260920`。
- 原两分支引用不移动；A 未提交文件另存外部备份与 stash，不自动 apply 到新主线。

## 主线程裁定（整合 r4 审核后）

1. 保留 B 已审查的 794 个源码/测试/文档路径和 6 个主线程路径；另纳入本事务记录。
   不使用 `git add -A`，生成物、原始证据、二进制备份不入提交。
2. 自动模型灯的五个源文件允许作为已有默认关闭研究代码进入源码 checkpoint；
   producer/consumer 均需显式环境开关1，不能称为本版已验收功能，不改变当前默认。
   不删除被现有 JAPI/测试引用的代码以“清理配置”。正式包仍须检查默认与能力说明。
3. 当前 buffer 初始 allocation 检查、初始化后注册、未移交 VkBuffer RAII 及具名失败处理
   随 B 原样整合；本次不新增 allocator 政策，不提高384MiB预算，也不改变GPU退役。
4. A 的旧图像迁移 guard 仍是未审完研究，不移植；AA/SSAO 等采用 B 已有 owned-layout 实现。
   两份 A 独有 Stage13 历史测试与研究脚本保存在旧分支/外部备份，不据此宣称测试已等价替换。
5. StormBreaker 保持原位，禁止 r3 提出的搬目录再 submodule update。其 index gitlink 与 B 相同，
   但实际子仓 HEAD/dirty 不同；外部备份、完整文件指纹及 `.git/index.lock` 均须原样保留。
   原主目录整合后可能仍显示该子仓 dirty，这不等于 superproject 源码冲突。
6. 所有 DSH 写者冻结；额外 r5 只读审查不得改文件/Git。主线程独占此次 Git 写事务。

## 备份、检查与完成定义

- 新备份：`D:/WarVK-Backups/20260920-formal-integration-A` 与 `-B`，CreateNew ZIP/逐文件SHA/CRC、HEAD bundle均验证。
- 子仓独立备份：`D:/WarVK-Backups/20260920-release-closeout-A-StormBreaker`，不得删除或覆盖。
- 交易前重跑冲突/ignored/reparse检查；49个已知untracked冲突必须完整进入stash。
  使用 `git switch --no-overwrite-ignore`，不使用force/reset/clean，不递归切子仓。
- A的旧ignored构建产物原样保留，但**不得继续作为v1.22构建目录/正式DLL使用**。
  A的StormBreaker旧dirty也不用于发布构建；正式验证须独立clean配置和正确依赖。
- 验证新主树全部tracked内容对应B冻结Git blob；A原分支/stash/备份可达，旧ignored及子仓不变，
  玩家DLL和B离线DLL不变。换行规范化与原字节备份分开记账。
- 此次树审计25、配置20、包审计25项独立纯Python测试通过，不是最终产品运行门。

## 状态

**本地主树源码整合已完成；不是稳定版本接受或发布。**

## 实际执行回执

- B在新checkpoint分支提交`17ecf66cb97368bf3ff033c5f0061d2f94c33da3`：801个文件、
  +150268/-3601行，包含此前累积的实现/测试/文档，不是本轮新增的研发量。
- 创建整合分支指向该commit，并在A执行非强制、不递归子模块、禁止覆盖ignored的switch。
  当前权威主目录为`E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk`；
  B保留在checkpoint分支，后续不再双树并行写产品源码。
- 旧A分支仍为`88089cdf90f728e85b91bf45d75002348574b665`；旧B release-integration分支
  仍为`ae890542d766470d1703f5bea7f5b73636039733`。保留型stash为
  `7b1a52a8535fcd300f7af51773edc9a2ef1a3616`，未apply/drop；50个实际changed tracked文件
  与120个untracked文件均核验可恢复（9个untracked仅Git换行规范化，外部ZIP保留原字节）。
- 切换后2870个index条目与冻结B完全一致；2866个普通文件逐字节核对，995个只存在CRLF/LF
  规范化差异，其余原字节一致。接着的本回执/入口/发布文档更新单独提交，不改产品代码。
- A的11542个ignored文件与StormBreaker的2404个文件逐size/SHA不变；子仓HEAD、status、
  `.git/HEAD/index/index.lock/config`不变。superproject切换后唯一原有dirty为StormBreaker，
  它是受保护的既有内容，不是未解决的合并冲突；不得据此使用该旧子仓做产品构建。
- A旧DLL、B内部676 DLL、玩家FAC DLL均不变；玩家FAC为36412825B /
  `FAC75C10D640F011BA07482B1706E77223756095BCFFCA448046B2FA0289E529`。

### 过程中遇到的真实问题（不隐藏失败）

1. 首次A stash因superproject `.git/index.lock`存在而失败，无A源码变动。确认没有Git进程、
   该锁为9月16日留下的0字节文件后，仅改名保存为`index.lock.parked-v122-20260920`，未删除。
   StormBreaker内自己的index.lock完全未触碰。随后stash成功。
2. 首次stash原字节断言发现Git自动规范化文本换行；用外部ZIP原字节与stash内容逐项比较后，
   仅允许CRLF→LF这一差异，其它内容仍要求完全一致，未忽略真实内容变化。
3. 首次完整cached diff空白检查exit2，发现历史未跟踪文档/源码注释/冻结参考夹具中的尾空白和
   EOF空行。保留其历史字节（参考夹具有SHA合同），不伪报该检查通过；本次新收尾diff另行检查。
   这是源码checkpoint的已知整理项，不等于C++/测试失败，也不授予发布资格。

### 验证、证据与后续

- A新主目录独立复跑：树整合25、配置20、包审计25、native-light bridge6/transaction18、
  Stage13 retention4、Render Stats7，合计105项通过。均为定向纯Python/static，不是全量回归。
- 完整CreateNew回执：`D:/WarVK-Backups/20260920-formal-integration-transaction/`中的
  `preflight.json`、`staged-index.json`、`stash.json`、`switch-verified.json`；外部A/B备份及旧子仓备份保留。
- 无新编译、部署、游戏/GPU、push/tag/release。版本资源仍未晋升1.22.00。
  下一阶段从本地主线commit建立独立clean构建与正确依赖，执行正式配置、组合视觉/GPU、包验收。
  旧A build32不能因源码已切换就冒充新DLL，旧B内部DLL亦不是正式配置产物。

### 回退入口

保留上述原分支、stash和ZIP。若需恢复旧A工作：先停止当前主树写者、备份之后的新修改，
再非强制切回`codex/native-shadow-stable-baseline-20260830`并apply指定stash，不能把stash
apply到v1.22分支。不drop、不prune，不将本说明当作自动回退指令。
