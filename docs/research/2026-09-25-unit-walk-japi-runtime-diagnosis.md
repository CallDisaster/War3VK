# 单位 Walk API：玩家地图无效果的定位与隔离回归

日期：2026-09-25。此记录只证明当前 1.27a / `Hamg` 样本的 JASS→DLL→自动 Walk 请求链；不等于动画视觉、其他单位或发布验收。

## 原始现场（只读）

- 玩家目录 `E:/Work/Warcraft III/d3d9.dll` 与先前构建完全一致：36,612,087 B / SHA-256 `FF3B7BD521F95935DB204EF2FEBAB828BDE85BA743E509292B9FA9F91B05F003`。`Game.dll` 为 13,187,048 B / `E04D1716603C075EB0C8E1E21CF1093A664ADC5249EFAB396BFA08D7B09D0C3A`。
- 玩家地图 `Maps/Test/WorldEditTestMap.w3x` 为 SHA-256 `AB4402A0EB07176B9858707A1BE3929586281781EE95626792F459642C56C3D4`。用现有 32 位 StormLib 提取 `war3map.j`，提取前后地图哈希相同。生成脚本第 1590 行确有 `WarVKSetUnitWalkLoopEnabled(gg_unit_Hamg_0033, false)`；目标单位由 `CreateUnit(...,'Hamg',...)` 建立。脚本**没有**紧接着调用 `SetUnitAnimation` 或两个 Override 助手，所以即使关闭成功也不是“立即播放替代动画”的请求。

## 原候选的失败事实

对提取的地图脚本仅在隔离副本里加入 30 秒后移动、手动 Stand、恢复开关，PJASS 通过；原地图未回写。候选地图 5,881,596 B / `DA8C842BB5FE22D0F3AEAAAFDCA7DA8C3FA7604A9F10FA800A1E2BEA900EC0BC`，脚本 MPQ 读回 SHA 与输入逐字一致。

新添的 CPU-only 内部测试快照（仅 `DXVK_WAR3_INTERNAL_TEST_API=1` 采样）把安装、setter 接受/拒绝、自动 Walk 请求与实际抑制分列。第一次隔离实机：安装状态 5（成功），JASS setter 调用 2、接受 0、拒绝 2，失败码 4（handle 对象类型不符）；游戏自动 Walk 请求 5、抑制 0。于是“DLL 没收到请求”被排除：它收到并拒绝了请求，故之后 Walk 全按原版执行。

第二次只细化拒绝观测：JASS handle 为 `1048796`；该 handle 表项在 `+0x10` 读到的值为 `0x00000AEF`（不是 CAgent 的 `+w3u`），但同一对象的 `CUnit+0x30` rawcode 是 `0x48616D67` (`Hamg`)，其虚表 `+0x8C` 指向 `Game.dll+0x67BB90`，与 IDA 逆向的 CUnit 动画属性方法完全一致。根因：`ResolveExactUnit` 原先**只接受 handle→CAgent→CUnit**，而当前 1.27a 实际是 **handle→CUnit 直接表项**。

## 收窄修复与复测

仅在 handle 表项的 rawcode 为可打印四字符、其虚表 `+0x8C` 精确指向已核对的 `Game.dll+0x67BB90`、且 `UnitWrapper` 可读时接受直接 CUnit；CAgent 原分支保持。Hook 在每次抑制前仍重新解析同一 handle，要求对象指针与保存身份相符且单位未死亡。其余指针和不匹配的 Game.dll 均 fail-closed。宿主机生产共用 predicate 测试覆盖正确 `Hamg`、坏 rawcode、错虚表目标与无效基址。

修复构建 DLL：36,618,365 B / `1FFE0BD14C51C8E1D2A9EB94425A06260CD4B480A7CA735E13E946EA93EE35CF`，PE `pei-i386/i386`。隔离桌面、零全局输入的两次独立用例均到达游戏 ready，完整结束后停止自有进程，玩家 DLL/地图/`Game.dll` SHA 未变：

同一 DLL 的 fail-if-exists 冻结副本位于 `AutoTest/artifacts/unit_walk_handle_fix_20260925_candidate.dll`，复制前后 source/target SHA 与 size 完全一致；目标仍由 `AutoTest/.gitignore` 忽略。这是待进一步视觉/跨单位验收的候选，不是已部署的玩家更新。

| run | JASS setter | 自动 Walk 请求 | 抑制 | 恢复后照常执行 | 证据 |
| --- | --- | ---: | ---: | ---: | --- |
| `unit-walk-probe-20260925-c` | 1 次关闭 + 1 次恢复，2/2 接受；关闭时当前属性为 Stand(4) | 576 | 574 | 2 | `walk-probe.json` 42,039 B / `7FE44720EA178713C253B6A909B16EB3773CD5F921485B3A7183DE5102C0CAE9` |
| `unit-walk-probe-20260925-d` | 同上 | 536 | 534 | 2 | `walk-probe.json` 42,841 B / `79D9F5FBB7BF96EC7EAFDB0C2FF464764715FFBE789BFAC819B321C97CFC6ABA` |

“恢复后照常执行”这一列是**全局 Hook 计数**，尚无对象键证明这两次属于该 `Hamg`，不能写成同对象恢复证明。在关闭期被抑制的请求读到的当前动画属性仍为 Stand(4)。run d 另保存了移动期手动 Stand 与恢复后的两张 BMP；它们证明帧被捕获，不能仅凭两张静帧认证动画循环或全模型视觉一致。对应图像在同一 artifact 目录，来源和哈希记录于 `walk-probe.json`。

## 仍未证明

- 玩家目录仍安装旧 `FF3B…` DLL；修复的 `1FFE…` 没有部署给玩家，也未写回原地图。
- 仅验证了 `Hamg` 直接表项样本。其他单位/召唤物类型、死亡与句柄复用、多人同步、跨地图会话仍需回归；当前裸指针 + JASS handle 复核不是完整 ABA 代际证明。
- API 目前拦的是**自动选回 Walk**，不主动停止已经在播放的 Walk，也不自动播放新动画。玩家原触发只调用 `false`；如需移动中看到明确替代动作，还必须调用原版 `SetUnitAnimation` 或 WarVK 的 Override 助手。移动中途关闭时现有 Walk 可能继续到自然切换，这个语义尚未覆盖，不能宣称已完整实现“停止 Walk 循环”。
- 这次 direct `-loadfile` 隔离测试只验 WarVK 的内置 Preloader 桥和纯 JASS 探针；没有借此宣称 YDWE 遗留 JAPI 地图或编辑器保存链已通过。最终玩家视觉接受仍需匹配 DLL、实际地图、目标单位与动画序列测试。
