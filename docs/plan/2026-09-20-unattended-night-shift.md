# 2026-09-20 夜间四线推进与主线程验收

## 目标与期限

用户授权主线程无人值守协调 DSH，推进 v1.22 剩余长期收敛、预算/内存优化、保守剔除与独立测试；不是承诺在截止前把全部长期计划完成。
台北 2026-09-20 08:20 停发新长批，08:30 开始冻结/验证/恢复/归档，09:00 结束并汇总。未达门项明确未完成。
主线程：`01a02e0b-1d1e-7762-b40b-63a00bbb3449`。唯一可写 B 树：
`E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk-v1.22-integration-20260914`。

## 备份与当前基线

- `D:/WarVK-Backups/20260920-night-0238`：HEAD bundle 已 verify；882 个 dirty 路径原字节 ZIP 已 CRC 与逐文件 SHA 复核，缺失路径记 manifest；tracked binary diff、status 与 DLL 一并保留。没有 commit/reset/stash/clean。
- HEAD `ae890542d766470d1703f5bea7f5b73636039733`；dirty ZIP 35,895,379 B / `853ECEACD4E89152825BD97B7704DB40562256C337DC032F7B35297E6D907555`。
- 起点 DLL 36,386,190 B / `68176AE78808CA2329EB9513502D805426401FC91101134E11C71C104F3C5378`；P1b/P2a 默认关，旧 89/267 是起点记录，不自动算新批次验证。
- 开工无游戏/编译进程；E 有约58GiB，不因获得清理权限就删除坏帧与历史证据。

## 四线与边界

| 线 | DSH session | 首批交付 |
| --- | --- | --- |
| architecture | session-5843bd3a-456c-4b8c-ad84-93bbd4f3c44a | S2 palette 适配层重复扫描/容量复用成本收口，保留数值/失败合同 |
| memory | session-151b7705-2f1d-40e9-924e-2616fc8a976b | P1b账户审查、同共享预算的寿命分页纯组件与反例，不提前接生产 |
| culling | session-e24b90ff-4d98-4d31-bbac-10cf1db2a9de | 实际剔除覆盖、早期减量条件、离屏投影/未知bounds反例与可审核提案 |
| validation | session-5818cc96-b042-433a-998d-81f8319f49b6 | runner与恢复/比较器纯测试、上一轮map-ready失败调查、独立OFF/ON矩阵 |

四线均为 commandcode / deepseek/deepseek-v4.1-flash；工作包给多个可验证里程碑，不以小修小补频繁打断主线程。
精确白名单、turnRef、lease、报告与回执：`AutoTest/artifacts/night-shift-20260920/coordination.json` 和 `workorders/`。
主线程独占共享 device / shadow draw / Meson / perf / AGENTS / 总日志集成；architecture 对 shadow-core 仅获 TryBuildRuntimeGroupPalette 局部权限。
子线程不自动扩文件、不再委派、不拿用户全夜授权直接操作系统/现场。没有 lease 时仅纯Python/static；编译/游戏分别单拥有者。

## 回报循环

Skill：`D:/Appdata/.codex/skills/warvk-dsh-night-shift/SKILL.md`。quick_validate 与回执脚本测试通过。
可发现真实 Codex 消息工具则直接回报；否则 CreateNew 信箱 + DSH final，由主线程 wait_turn 接收，heartbeat 补查。
不猜 localhost RPC、不另起 app-server 抢占线程、不编辑会话数据库。当前已确认 DSH send acceptance；直接 Codex 推送尚未验证。
DSH 工具的旧长连接凭据失效；新 `scripts/dsh_call.mjs` 读取既有 config 凭据到内存运行 MCP bridge 已验证，不重启共享服务、不打印令牌。
监督沿既有 automation `warvk-2` 更新为每10分钟，截止09:00。每次审查回执/真实diff/失败测试，给出修订或下批；不在闲置时只报状态而不推进。

## 设计与资源硬门

- v1.22 不引入 x64产品、未完成Water或自动模型灯。重点是现有32位路径正确性与低成本。
- 不提高384MiB默认预算、不靠漏投影/关闭阴影/延长旧图寿命通过；CPU cache空闲不等于GPU可覆写，保留epoch/fence/完整性。
- P3生产策略由真实账户决定；剔除必须区分捕获前节省与最终draw节省，不把相机外等同于无阴影贡献。
- 主线程冻结写者/哈希后给validation独占构建，父进程BelowNormal，最多-j2；禁止并发编译。
- 实机固定非交互隔离桌面、2560x1440、零global input，实际窗口/backbuffer/receiver分别取证，不达分辨率不伪装有效。
- 每次部署精确检查目标、候选、备份、地图与零进程，CreateNew备份、条件恢复；不得关闭用户编辑器或覆盖正在游玩现场。测试失败不能偷偷重试替换。
- 用户授权必要时安装环境/清理生成物；由主线程核定范围，禁止系统文件、源码/原始报告/坏帧/唯一备份删除。当前无清理必要。
- 不自动提交、合并、推送或发布；候选不冒充稳定。每个验证checkpoint更新开发日志；稳定日志须真正接受才改。

## 08:30 收口清单

停止新改动，确认所有lane回执；记录真实未完成项；串行相关测试/必要构建；结算任何游戏恢复；核零残留；列源diff、DLL身份、测试层级、内存/剔除实据与长期进度增量。
09:00暂停监督，向用户交付候选/报告路径和验收缺口。不能为准时而遗弃恢复或把未跑测试报PASS。
