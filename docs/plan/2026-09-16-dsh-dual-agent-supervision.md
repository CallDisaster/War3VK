# DSH 双线程：录制导出终态收敛与独立测试

状态：2026-09-16 12:36本批完成纯离线验收；两方停笔，20分钟heartbeat按完成条件暂停。
实际证据见开发日志同日收尾与`AutoTest/artifacts/dsh_dual_supervision_20260916/checkpoint-final.json`。
不代表产品DLL/实机/闪退验收；下一批仅建议，未自动派发。下文保留本批原始范围与监督合同。
本轮由原夜间heartbeat改为此新监督任务，旧08:00截止授权不再沿用；本文件只授权下述离线范围。

## 基线、备份与所有者

- 唯一写入树：`E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk-v1.22-integration-20260914`。
- DSH工作区仍是旧`E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk`，旧树只读；所有编辑必须使用集成树绝对路径。
- 原分支`codex/v1.22-release-integration-20260914` / HEAD `ae890542d766470d1703f5bea7f5b73636039733`，开工274 dirty。
- 用户明确授权本地Git备份：`codex/backup-before-dsh-dual-20260916`，快照commit `30c9163ea85081b367b114bb5e872362fb5dd065`。
  这是未验收WIP备份，不是产品提交/发布。原HEAD、真实index、274路径字节均未改动。
- 独立备份目录`D:/WarVK-Backups/20260916-dsh-dual/`；root bundle、四个submodule bundle已verify；
  raw ZIP包含274路径及imgui的未跟踪meson.build共275文件，逐条重算SHA一致。
  receipt=`backup-receipt.json`。忽略的DLL/build/运行证据不在此源码备份中，留在原处未修改。
- 主线程：设计约束、接口签发、差异审核、编译协调、验收及总日志；两个子线程不得修改本计划、总日志、Git或彼此文件。
- Kimi：`session-19569f28-5d7b-4da2-939c-25203e44f292` / 当前`cli-proxy-api / kimi-k3`。
  2026-09-16 12:28用户因额度要求切换供应商，MCP已确认；原会话与成果保留，禁止自动改回scnet。
  此前两轮实施在原scnet配置下完成；新配置不能用于重标旧结果的来源。原dispatch回执保持历史原样。
- GLM：复用`session-a9b8cb49-7270-4234-898a-93ff8849114b` / `scnet / GLM-5.3-Flash`。
  当前两款配置均无独立reasoningEffort选项，不擅自替换。

## 本批真实问题与验收目标

先读`AGENTS.md`及`2026-09-15-incremental-architecture-convergence.md`，核心依据为
`2026-09-16-recorder-export-terminal-contract.md`。旧文档“设计尚未授权实施”的状态由本次用户授权与本计划更新：
允许本批实现候选，但不得违反其完成定义、锁序、GPU寿命或证据范围。

status与nextExport现在各自判断导出完成，副作用不一致。要将采样及终态/HUD/错误提交收拢，
消除status先完成时绕过HUD/fault更新的路径；不是拆文件凑数量，不是GPU闪退/阴影修复。
图像Complete不等于整个packageReady；不更改GPU资源、fence、registry、join、预算、窗口或wire schema。

## 冻结的跨线程接口（主线程签发）

新增头`src/d3d9/war3/tools/war3_frame_history_export_core.h`，namespace `dxvk::war3::tools::history`：

```cpp
struct ExportProgress {
  uint32_t retained = 0, queued = 0, done = 0, failed = 0;
  bool requestsValid = false;
};
enum class ExportOutcome { Pending, Complete, ReadbackFailed, InvalidProgress };
inline ExportOutcome ClassifyExportProgress(const ExportProgress&) noexcept;
```

纯CPU/无分配、无OS/GPU/日志调用；MaxSlots沿用已有history core，不能复制一个会漂移的容量常量。
语义顺序：requestsValid=false、retained=0或>MaxSlots、failed>done、done>queued、queued>retained均InvalidProgress；
合法且done<retained为Pending（即使failed>0也等待全部结算）；全部done后failed>0为ReadbackFailed，否则Complete。
不做溢出加总。类型及以上语义未经主线程复核不得改变；若有冲突停止并提出精确替代，不自行降低测试。

生产唯一提交入口名固定`Impl::settleExportProgressLocked`，签名/返回观察数据由Kimi设计；由status和nextExport调用。
Kimi可在同一新头中设计用于生产和真实CPU交接测试共用的最小提交边界，不创建第二套状态管理器。
先acquire读取done，再读取其queued/success；每次调用至多一次有界全表采样，可同时记下下一待排队项。
校验orderCount/order index/request后才读；只在Exporting且未取消/停止时提交，不复活Fault/Discard。
非法进度只提交CPU故障，不凭此释放在途资源；HUD多个原子不冒充跨字段事务快照。

## Kimi：核心架构实施（仅4路径）

允许apply_patch：

1. 修改`src/d3d9/war3/tools/war3_frame_history.cpp`，限导出进度采样/唯一终态提交/相关include与调用。
2. 新增上述`war3_frame_history_export_core.h`。
3. 新增`AutoTest/test_frame_history_export_commit.cpp`，验证实际生产共用提交边界，不复刻另一套模型。
4. 新增`docs/agent-history/2026-09-16-dsh-export-architecture.md`，先写精简设计及状态/锁序图，再实施并记录结果。

必须覆盖两种入口先后顺序、全部成功、部分失败待结算、全部失败收口、重复查询、取消/停止不复活、
图像完成不签发packageReady及非法人口。若真实提交边界不能在上述范围内测试，报告具体缺口，不能伪称已验证。
不得修改GLM测试、原有测试、其他C++/header、Meson、shader、生产配置、总开发日志。
可运行纯Python/static、diff/status/hash；本批不执行编译器/Ninja/native runnable/游戏，编译验证由主线程后续串行管理。
自己的新输出仅可在`AutoTest/artifacts/dsh_export_architecture_20260916/`，不覆盖任何既有证据。
完成本批即停止写入，提交四文件精确差异/身份/已跑与未跑项，不自行转入下一模块。

## GLM：独立机械测试（仅4路径）

允许apply_patch：

1. 新增`AutoTest/test_frame_history_export_core.cpp`，直接include实际生产头，对固定接口做表驱动正反例。
   覆盖四种结论、0/MaxSlots/MaxSlots+1、UINT32_MAX、各人口逆序、request缺失、部分失败与待排队、边界稳定性。
2. 新增`AutoTest/test_frame_history_export_contract_static.py`，仅做明确标注的生产接线静态检查：
   status/nextExport调用唯一入口；原重复终态分支消失；schema/状态编号/关键原所有权边界保留。
   必须限定函数体而非全文件字符串计数；静态检查不能冒充运行时/CPU提交测试。
3. 新增`docs/agent-history/2026-09-16-dsh-export-tests.md`，记录测试矩阵、调用命令、失败及限制。
4. 可修正自己上一份`2026-09-16-dsh-mechanical-batch-01.md`中的事实错误：实际15个测试都有stderr输出，
   并非全部静默；区分父线程py_compile与自身ast检查，不改旧receipt或伪造新结果。

不改Kimi生产代码/接口/commit测试、不扩旧白名单、不改原测试阈值、不改总日志。
可运行纯Python/static/语法检查；不运行编译器或native gate。本批C++测试由主线程后续编译。
若生产头/接线尚未落地，可写完测试后如实记录待集成，不写假生产实现，也不无限等待/反复跑失败测试。
只用新ignored目录`AutoTest/artifacts/dsh_export_tests_20260916/`保存本批结果。完成后停止并回传。

## 主线程20分钟监督流程

1. 先读本计划、开发日志最新checkpoint及ignored调度回执，确认两个DSH的model/session和实际状态。
   running不等于卡死；不因等待超时重发原任务，不让两人编辑重叠文件，不开启第三个实现者。
2. 只读审阅实际差异/路径/来源SHA，验证原274快照外的变更均属于本计划（主线程新增计划/日志/回执另列）。
   发现越界、源文件并发变化、伪验收或破坏所有权，暂停对应任务并报告，不用reset/clean抹掉证据。
3. 对完成/需纠正者读取final，给精确反馈和同一范围修订；仍运行且没有新可行动信息时结束本次唤醒，勿忙轮询。
4. 双方停止写入后，独立审代码、运行定向Python；允许主线程用既有编译器串行BelowNormal编译/运行
   这两份无GPU的CPU测试、必要的原CPU门和生产TU -fsyntax-only（保留输出隔离和源码身份）。
   不运行Ninja/dry-run/产品DLL构建、部署、游戏、GPU，也不接管玩家/调试器，不安装软件。
5. 任一验收checkpoint更新DEVELOPMENT_CHANGELOG，不把未编译、CPU成功、无阴影或未复现冒充实机修复。
   本次备份授权不包含自动commit/push后续实现；不得清理日志、自动回滚或改旧证据。
6. 完成两项并纠正审查问题后，向用户总结实际成果、剩余门和下一批目标，然后暂停本heartbeat。
   “总结新目标”不等于无限自主开新模块；下一批范围另行明确。无法安全继续或用户叫停时也暂停并报告。
   原始Git备份永久保留，恢复须先保留新改动并按文件审阅，不对dirty树执行reset --hard。

当前批次只授权以上候选实现与离线验收，不承诺FPS、内存收益、闪退修复或可发布DLL。
