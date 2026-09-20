# 实机对照前置之二：部署/恢复链已验证可逆 + 精确哈希台账 — 2026-09-18

> 承接 `...-live-contrast-plan.md`。本文 (a) 验证部署-恢复链可逆，(b) 记录**精确的完整 SHA**，
> (c) 记录一次**我自己的错误**（猜哈希尾部）与守护如何拦住它。

## 1. 精确哈希台账（**完整值**，不再用前缀代替）

| 对象 | 大小 | SHA-256 |
| --- | --- | --- |
| **现场 DLL（基线）** | 36,288,789 B | `F275545BAA65A0153804AC1CC0B3533B595DA24B5725488B8FFB9DDC5CF07FF3` |
| **候选 DLL** | 36,297,472 B | `BBEF3BBC8F52088FD34D6B5F68FEFA111BAA31A7CB2331674AC3917DE0DE357A` |
| 备份 `d3d9.dll.519AFA69_backup_20260918` | 36,283,128 B | `519AFA6904F47E9C2E168FF0E5E0A17A470B27F9F9E714783D99C0333E397306` |
| 备份 `d3d9.dll.A0A51AF2_backup_20260918-092843` | 36,271,456 B | （未复算） |

## 2. 部署/恢复链验证（try/finally 保证恢复）

```
pre-deploy match baseline : True
backup created            : d3d9.dll.selfcheck_backup_<stamp>
deployed SHA              : BBEF3BBC…DE0DE357A
DEPLOY VERIFIED           : True   (= 候选 SHA)
restored SHA              : F275545B…5CF07FF3
RESTORE OK                : True   (= 部署前 SHA)
final len / SHA           : 36,288,789 B / F275545B…5CF07FF3
```

⇒ 换入与换回**均以 SHA 逐位核对**，且恢复放在 `finally` 中 ⇒ 即使中途出错也会恢复。
**现场最终状态与基线逐位相同**，本轮未留下任何改动。

## 3. ⚠️ 我自己的错误：猜哈希尾部

第一次运行守卫脚本时，我用的期望基线 SHA 是 `F275545BAA65A015` + **我凭印象补的尾部**，
实际完整值为 `F275545BAA65A0153804AC1CC0B3533B595DA24B5725488B8FFB9DDC5CF07FF3`。

**结果：守卫直接 `ABORT` 并 `exit 2`，未复制、未覆盖任何文件。**

⇒ 这正是"部署前必须核对 SHA"这条纪律的价值：**它拦住了我自己的错误，而不是拦住现实中的意外**。
教训：**永远不要把 16 位前缀当作完整哈希使用**；需要完整哈希时先复算并记录。
（此前多轮报告中我以 `F275545B…` 指代现场 DLL，含义模糊；本文给出精确值作为唯一权威。）

## 4. 剩余工作（唯一一项）

现在这条链的**可逆性已证明**，剩下的只有"真正跑一次游戏并采集导出"：

```
4. 隔离桌面启动（direct / isolated / windowed / deploy_d3d9_before_launch=False /
   env: FRAME_EVIDENCE=1 + PALETTE_OBJECT=1 + HISTORY_SELF_CONTAINED=0 /
   地图 (2)ConcealedHill.w3x）
5. 触发取证导出（arm|trigger|freeze|export 或热键）
6. 解析统计 stage / terminal / counters
7. 恢复现场（机制已验证）
8. 对照运行以判定「正常对象是否被误伤」
```

注意：步骤 4 之前**仍然**没有实机非零导出 —— 批次 3 的 FirstSight 链同样依赖这一步。

## 5. 状态

| 项 | 值 |
| --- | --- |
| 全量静态 / meson / runnable | 258-0 / 85-0 / 74-76 |
| 现场 DLL | **未改动**（与基线逐位相同） |
| 部署-恢复链 | **已验证可逆**（本轮） |
| 实机运行 | 未做 |
| 未提交 / 未部署 | 是 |
| 目标 | **不可标记完成**（第④问第四子问题仍无运行期证据） |