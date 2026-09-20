# 阶段 E · 实机采集协议（**v4 契约版**）— 2026-09-18（round 78）

> 取代 `2026-09-18-real-machine-evidence-session-protocol.md`（该文描述已不存在的部署状态）。

## 0. ⛔ 授权前提（**未获授权前，一步都不做**）

AGENTS.md 硬约束：**构建或测试不会自动授权部署 DLL、覆盖 YDWE/Warcraft 文件、启动/关闭编辑器或游戏。**
⇒ 下列任何一步都需要**用户明确请求**。当前状态：**站点 = 基线 ⇒ 未部署**，**未启动任何进程**。

## 1. 白名单（**从源码机械枚举**，不是猜的）

**必填**（缺失 ⇒ 证据子系统**零操作**，采集会**静默为空**）：

```
DXVK_WAR3_FRAME_EVIDENCE
DXVK_WAR3_FRAME_EVIDENCE_PALETTE_OBJECT
```

**上下文旋钮**（记录当时值；本协议不设置它们）：`..._CASTERS/_DRAWS/_INPUTS/_OUTPUT`、
`DXVK_WAR3_FRAME_HISTORY_SELF_CONTAINED`、`DXVK_WAR3_SEMANTIC_PALETTE_DIAGNOSTICS`、
`..._SHADOW_METADATA_*`、`..._SHADOW_POSE_FULL_TRACE`、`..._SHADOW_STAGE_LIFECYCLE`、
`..._NATIVE_DOODAD_STATIC_STAMP`、`..._BLOCK_NATIVE_DOODAD_STATIC_SHADOW`、
`..._PERF_LEVEL/_MONITOR/_RECORD_ON_START`、`DXVK_WAR3_DISABLE/_PROFILE/_INTERNAL_TEST_API`。

**并且记录全量 `DXVK_*`** —— 理由：**漏记本身就是盲点**。

## 2. 启动环境快照（**装置已就绪并实跑验证**）

```powershell
.\AutoTest\capture_startup_environment.ps1     # 只读；不部署、不启动、不设置任何变量
```

产物：`E:\Work\warvk-capture\startup-env-<UTC 时间戳>.json` 与 `.md`（含 OS/CPU/**全部**显示适配器+驱动、
**两份** d3d9.dll（开发树 + 站点）的字节数/SHA-256/mtime、UTC 时间戳、主机、用户）。

已实测：变量名正确渲染、缺失必填项**具名**报告、站点 DLL 哈希与基线逐字相同。

## 3. 采集产物

| 产物 | 位置 |
| --- | --- |
| 对象级导出 | `E:\Work\Warcraft III\WarVK\Log\FrameEvidence\cpu-<PID>-<nonce>-<generation>.json` |
| 伴随输入 | `*.json.inputs.bin` / `*.json.inputs.json`（**几十 MB，核查不需要**） |
| 启动快照 | `E:\Work\warvk-capture\startup-env-*.json/.md` |

## 4. ★ 必须核对（**v4 契约**；旧协议缺这一节）

| # | 核对项 | 期望 | 为什么 |
| --- | --- | --- | --- |
| 1 | `effectiveConfiguration.paletteObjectEvidence` | `true` | 子门是否**真的在游戏进程里**开启（曾因它为 false 导致整轮无效） |
| 2 | `paletteObject.version` | **`4`** | 阶段 C 后写方**恒定 v4**；出现 2/3 说明跑的不是当前候选 |
| 3 | 链型槽 `data[4]` | `0`=RejectionRecovery / `1`=Observation | v4 起该槽从保留零改为链型载体；**v1–v3 必须仍为 0** |
| 4 | 终态取值 | `0..7`（新增 **`ObservationClosed=7`**） | 终态表**按版本**校验；7 只在 v4 合法 |
| 5 | `windowSegment` | 窗口内一致；跨窗口不同 | 查找维度 = 对象键×链型×**窗口** |
| 6 | `attemptSerial` 语义 | 「一次尝试」= **一条清单记录**（record 级） | **不是**每帧清零；同一尝试在 Served/Reject/FirstSight 三点应得**同一个号** |
| 7 | `counters.emitted` / `terminalEmitted` | `emitted > 0` | 是否真的产生了对象级记录 |
| 8 | `watchCount` / `dropped*` | 记录，不设阈值 | 是否有对象在跟踪、是否被丢弃（预算/终态保留/逐帧） |

**判成功（最低）**：`paletteObjectEvidence == true` ∧ `emitted > 0` ∧ `version == 4`。
**不要求**全部计数器非零；**不**因 `identityNotProven` 判失败（那是**有意收紧**）。

## 5. 明确**不能**从导出推断的东西

- 单次导出**不能**证明"阴影已恢复"或"症状已修复"；
- CPU 侧对象级导出**不是** GPU 提交证据，也**不是**画面正确性证据；
- **隔离桌面**（isolated desktop）数据**不得**当作玩家前台性能；
- **一张表不代表一种事实，一条链结算不代表观察完整，观察完整不代表对象已证明**；
- 两个 P0（B/C 与 D）完成前**不新增实机因果结论、不晋升稳定候选**。

## 6. 现场状态与回退

```
站点 d3d9.dll = 36,288,789 B  F275545BAA65A0153804AC1CC0B3533B595DA24B5725488B8FFB9DDC5CF07FF3
             = 基线 ⇒ **当前无需回退**（未部署）
```

站点目录内另有 **7 个 `.locked_*` 残留**（见 `2026-09-18-site-directory-inventory.md`）——
它们是**改名让路（rename-park）**机制的产物，**已知、已记录、无害**；
按 AGENTS.md，**我不删除**它们（需用户明确请求）。

## 7. 若将来获授权部署

1. 先核对目标进程与**精确备份/哈希**（AGENTS.md）；
2. 保留回退步骤（站点的 park/restore 机制已自测，见 `2026-09-18-restore-tool-selftested.md`）；
3. 部署后**立即**跑 §2 快照，把「部署的到底是哪一份」写进工件（因为 **DLL 哈希含构建时刻，不是源码指纹**，见 `2026-09-18-build-not-reproducible-measured.md`）；
4. 不得在玩家游玩期间替换/删除文件或启动构建。

## 8. 不声称

- **不**声称本协议已被执行过（**零实机采集**）；
- **不**声称白名单**完整**（它从源码机械枚举；额外记全量 `DXVK_*` 兜底）；
- **不**声称 §4 的 8 项核对**足够**认定正确（它们只是**最低**核对集）；
- 全局边界：一张表不代表一种事实，一条链结算不代表观察完整，
  观察完整不代表对象已证明，更不代表阴影已恢复。