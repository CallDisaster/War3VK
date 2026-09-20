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

本条写入时尚未执行 Git mutation。实际提交、切换、验证与未覆盖门在收尾回执中追加；
不得把此方案当作已合并证明。v1.22正式配置、独立构建、组合视觉/GPU与发布包仍待完成。
