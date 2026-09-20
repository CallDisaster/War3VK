# 2026-09-19：压力 canary 未通过进图门，恢复完成；合并条件未满足

## 裁定与范围

用户授权本轮测试，且仅在修复与测试完成后进行发布准备、合并 v1.22 独立树。
核验发现上一轮只完成 P0/P1a 首批离线候选，P1b/P2/P3 尚未完成。因此本轮只运行一次
早期有界诊断，不把它当作最终 P4；**未合并、未提交、未发布、未修改 C++ 或重新构建**。
64 位产品接入、未完成 Water、自动原生模型灯仍排除。本轮没有用增大预算或少画 caster 充当修复。

## 实际运行与失败

| 尝试 | 实际游戏进程数 | 结果 |
| --- | ---: | --- |
| `snapshot_pressure_bf938_20260919` | 0 | B 树 API 不接受 `expected_map_sha256`，启动前失败；现场已恢复，失败回执保留 |
| `snapshot_pressure_bf938_20260919_interface_r2` | 1 | PID 18904；隔离桌面启动成功，100 秒地图 ready 超时；路线未执行 |

第一项是驱动接口错误，不是游戏崩溃。修订时增加实际函数签名 bind 测试，并坚持导入 B 树
`war3_autotest_mcp.py`，不借用 A 树同名但不同签名的 runner。

第二项冻结 BF938 候选、F2A7 现场备份、Game/EXE/map 身份后执行。地图为
`E:/Work/Warcraft III/Maps/(4)生与死v1.28读档bug修复.w3x`，原地加载，无替换地图。
主录制/图像历史/原始输入取证关闭，full_default、384 MiB 原预算，自动 perf 导出 20 秒；
后台降载/自动暂停禁止，零全局输入，不切输入桌面。EXE 已有 LAA=true，如实记录，未修改 EXE。

- 客户区先得到 2560×1415，随后仅修改本事务隔离窗口样式达到 **2560×1440**；
  DXVK log 的最终实际 swapchain 同为 2560×1440。没有取得本轮 BMP 或游戏 receiver viewport，不能冒充三方门通过。
- ready 最后读数：`jassReady=true`、`gameStarted=false`、`runtimeReady=false`、
  `isInGame=false`、`isLoading=false`、`worldPtr=0`。主菜单类记录仍更新；没有压力/解除路线、没有新性能 HTML。
- 未抓失败画面，故不能据此判定加载确认页、地图兼容、renderer 崩溃或预算失败的具体原因。
- `game.end_for_exit_test` 只证实 stock EndGame 已调用，**不等于进程退出**。
  20 秒后仍存活，原 receipt 如实保留 residualPid，transactionOk=false，不事后改写为成功。
- 后续只核验并关闭本次进程：原 PID + 创建时刻 + canonical EXE + 保留进程句柄，
  工作线程绑定本次非交互桌面，对该进程唯一游戏窗口发送 WM_CLOSE，等待 exact HANDLE 退出。
  没有强杀、全局输入或桌面切换。随后按 launch 前值恢复视频注册表。

## 恢复与证据

候选：36,359,521 B / `BF938FBB6FBBB0B84A64EFE3A3E32545AAB98402D17180D2BDD6CA466A460CB3`。

测试目录恢复：36,358,788 B / `F2A7A6FC8FFAD077CEE70D69A2F0B16B48AC514837D64BC7D66CE3804F2CC33D`。
这是原现场身份，**不称为已验收稳定版**。
受保护 `E:/Work/War3/d3d9.dll` 仍为
`A0A51AF2BB9091B2C677DC34BEBDF76C02347049A89D0BB2AEF4C37518E52134`，未写入。
原 DLL 备份、两次 parked 候选均保留，可恢复，不删除用户证据。

证据目录：`AutoTest/artifacts/snapshot_pressure_bf938_20260919_interface_r2/`。
关键文件为 launch/ready/resolution/receipt、`manual-settlement.json`、`war3_d3d9.closeout.log`、
`offline-closeout-tests.json` 和追加的 `closeout-final.json`（3,963 B /
`25D1236E7A984652706EA8C32E8DE7400AE7C896626F04521DBA64DAEBDC85FD`）。
最后一份回执保留原失败并证明后续恢复，不覆盖原来的 residual 记录。
最后核验：相关游戏/编辑器/编译进程为 0，候选/EXE/Game/map 身份不变，未发现新增 dump 或相关 GPU 错误事件。

## 离线修正（不是本轮实机使用过的新驱动）

新增 `run_snapshot_pressure_canary.py` 及 13 项定向 Python 测试：

- strict JSON 根/递归重复键/非有限值拒绝；真实 B API 签名校验；CreateNew 备份。
- 避免直接截断 live DLL：同目录 CreateNew 暂存并核验，旧文件 rename-park，再将已验证暂存文件移入。
  Windows rename 不覆盖已有目标；第二步失败仅在 live 仍不存在且旧身份匹配时回滚。
  这不是断电原子交换；中断时文件仍保留可恢复，未承诺能抵抗不合作进程对 live 的任意写入。
- 故障注入覆盖部分写入、暂存中 live 被外部修改、发布 rename 失败、park 后外部文件抢占、旧目标已存在。
- 将此次确实奏效的隔离窗口正常关闭流程纳入后续驱动；先验证 owner，再由专用线程发送 WM_CLOSE。
  仅在 exact HANDLE 已退出后才允许调用现有 stop 的结算接口，不能把 `force=False` 当作避免强杀的充分保证。
  新封装仅离线门测试，未重新启动游戏验证；实机佐证仅为本轮独立 settlement 脚本。

13/13、对应 py_compile、仓库既有配置下 git diff --check 通过。
收尾脚本首版擅自覆盖 core.autocrlf=false，导致已有 CRLF 内容被 diff --check 判尾空白；
已撤销这个命令级覆盖，未改仓库配置或重写文件。该失败不算代码回归，也不隐藏。
此前 Meson 87/87、265 个静态脚本属于上一轮 BF938 离线证据，**本轮没有重跑或追认实机通过**。
实际运行的驱动源码已冻结在产物目录，与随后离线加固版本分别记录 SHA。

## 合并预检与下一步

B HEAD `ae890542d766470d1703f5bea7f5b73636039733`，A HEAD
`88089cdf90f728e85b91bf45d75002348574b665`，共同祖先 `8f232cc7d5a69bb4cadc99d41795273e6900b9f1`；
提交层 B/A 独有数 221/1。A 尚有 175 项 dirty，B 开工 867；本轮新增两个 Python 文件及本记录，
另更新既有 AGENTS/开发日志/实施计划的状态入口。**HEAD 相同不代表工作树相同，merge 不会自动带入未提交内容。**
本轮未改 A，未运行 checkout/reset/merge/commit，不覆盖其未提交资产。

发布和合并阻塞仍为：

1. P1b 默认关闭的页/切片持有者 census 与失败尝试/成功寿命对账。
2. P2 可信范围摘要，减少小 draw 冻结整个大 VB 的放大；不按 draw 扫非缓存映射。
3. P3 根据 census 落最小可证明回收，不凭 CPU 引用数重用 GPU 在途区间。
4. 先解决/查明测试进图链，再冻结新组合执行压力→解除→原地点恢复、完整性与像素门。
5. 正式发布配置、长期/Reset 门、v1.21 差异与明确排除项审查；用户视觉门不能由离线报告替代。
6. 真正满足条件时先备份并冻结两棵 dirty 树，以 B 为已裁定唯一主线整理受验收变更，再做受控集成。

当前结论：**编译资源已释放；游戏资源已释放。压力恢复未覆盖，阴影消失未验收，不能合并或发布。**
