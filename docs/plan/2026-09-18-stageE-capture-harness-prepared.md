# 阶段 E · 实机半的**预备装置**（白名单 + 启动环境快照）— 2026-09-18

> 裁定的 E 条目含「**白名单采集并保存完整启动环境**」。
> 本轮**只做预备**：装置写好并**实跑验证**；**没有**部署 DLL、**没有**启动任何进程。

## 1. 装置

`AutoTest/capture_startup_environment.ps1`（**只读**，不部署、不启动、不设任何变量）。

产出两份工件到 `E:\Work\warvk-capture\`：
  `startup-env-<UTC 时间戳>.json`   机器可读，供离线分析拼接
  `startup-env-<UTC 时间戳>.md`     人读版 + **具名**的缺失必填项报告

## 2. 白名单**从源码机械枚举**（不是猜的）

**必填（缺失 ⇒ 证据子系统零操作、采集会静默为空）**：

  DXVK_WAR3_FRAME_EVIDENCE
  DXVK_WAR3_FRAME_EVIDENCE_PALETTE_OBJECT

**上下文旋钮（记录当时值；本脚本不设置它们）**：

  DXVK_WAR3_FRAME_EVIDENCE_CASTERS / _DRAWS / _INPUTS / _OUTPUT
  DXVK_WAR3_FRAME_HISTORY_SELF_CONTAINED
  DXVK_WAR3_SEMANTIC_PALETTE_DIAGNOSTICS      ← 直接影响 palette 诊断载荷
  DXVK_WAR3_SHADOW_METADATA_CAPTURE / _ALPHA / _BLOCKER
  DXVK_WAR3_SHADOW_POSE_FULL_TRACE / DXVK_WAR3_SHADOW_STAGE_LIFECYCLE
  DXVK_WAR3_NATIVE_DOODAD_STATIC_STAMP / DXVK_WAR3_BLOCK_NATIVE_DOODAD_STATIC_SHADOW
  DXVK_WAR3_PERF_LEVEL / _MONITOR / _RECORD_ON_START
  DXVK_WAR3_DISABLE / DXVK_WAR3_PROFILE / DXVK_WAR3_INTERNAL_TEST_API

**并且记录全量 `DXVK_*`**（白名单之外也记）—— 理由：**漏记本身就是盲点**，
若某个未预料到的旋钮当时被设过，事后无法从工件里发现。

## 3. 快照还记什么

- OS（版本 + build）、CPU、**全部** `Win32_VideoController`（含驱动版本）；
- **两个** d3d9.dll 的字节数 / SHA-256 / mtime：开发树 `build32` 与**站点** `E:\Work\Warcraft III`；
- UTC 时间戳、主机名、用户。

## 4. 实跑验证（本轮实测）

```
json = E:\Work\warvk-capture\startup-env-20260918-164537.json
md   = E:\Work\warvk-capture\startup-env-20260918-164537.md
MISSING REQUIRED: DXVK_WAR3_FRAME_EVIDENCE, DXVK_WAR3_FRAME_EVIDENCE_PALETTE_OBJECT
```

⇒ 变量名**正确渲染**（早期版本会渲染成 `$n`，已修）；缺失必填项**具名**报告；
⇒ 工件里站点 DLL 哈希 = `F275545BAA65A015…`（**与基线逐字相同 ⇒ 再次机械确认未部署**）。

## 5. ⚠️ 一条机器环境**观察**（不是结论）

本机 `Win32_VideoController` 列出**三个**适配器：

  NVIDIA GeForce RTX 4060 Ti | driver 32.0.16.1692
  OrayIddDriver Device | driver 17.50.19.949
  MuMu Virtual Display Adapter | driver 20.36.41.498

⇒ 存在**虚拟显示适配器**。这对"隔离桌面/前台"类采集有直接影响：
**哪一块适配器在渲染**决定了采集数据属于哪个语境，
而裁定明确「**不把隔离桌面数据当前台性能**」。

**我把它记为一条待核对的观察**，不据此下任何结论（没有采集、也没有做过适配器归属的判定）。

## 6. 本轮**没有**做的事（硬约束）

- **没有**部署 DLL（站点哈希与基线逐字相同）；
- **没有**启动游戏或编辑器；
- **没有**覆盖任何 YDWE / Warcraft 文件；
- **没有**设置任何环境变量（快照是**只读**的）；
- **没有** git 写操作。

## 7. 不声称

- **不**声称 E 的实机半有**任何**进展（装置 ≠ 采集；**零实机数据**）；
- **不**声称白名单**完整**（它是从源码机械枚举的，但源码里可能有我未纳入的旋钮；
  因此脚本额外记录**全量** `DXVK_*` 以兜底）；
- **不**声称适配器归属已判定（只是一条观察）；
- **不**声称任何修复在实机上改变了症状；
- 全局边界：一张表不代表一种事实，一条链结算不代表观察完整，
  观察完整不代表对象已证明，更不代表阴影已恢复。