# 检查点①现状复核：c1 的指纹**不可复现**，当前须以本文件口径为准 — 2026-09-18

> 目标①要求检查点明确记录「**测的是哪份源码/哪个配置/回退回到哪里**」。
> 本轮试图刷新该记录，结果发现 **c1 的源码指纹无法用任何我试过的口径复现**。
> 这本身是检查点①的一个质量问题，如实记录。

## 1. 当前测量（口径已写明，可复现）

```powershell
$root = (Get-Location).Path
$src = Get-ChildItem src -Recurse -File -Force | Where-Object {
         $_.FullName -notmatch 'build32' -and $_.FullName -notmatch '\.git' }
$lines = $src | Sort-Object FullName | ForEach-Object {
           "$($_.FullName.Substring($root.Length+1))|$($_.Length)|$((Get-FileHash $_.FullName -Algorithm SHA256).Hash)" }
$h = sha256(($lines -join "`n"))
```

| 量 | 值 |
| --- | --- |
| src 文件数 | **964** |
| src 指纹 | `D829DAE2828BE6DEA0A6D7F8D9C64DCC10B42531A980B35B879D1DDB349A08A5` |
| 候选 DLL | 36,289,280 bytes |
| 候选 DLL SHA-256 | `CFE40FE5F929F40F94FB81F00B7AF118E2B24D93033EBC5FD0C7EDEEA5D0F31D` |
| **现场站点** `E:\Work\Warcraft III\d3d9.dll` | `F275545BAA65A0153804AC1CC0B3533B595DA24B5725488B8FFB9DDC5CF07FF3`（**未变更**） |

## 2. 为什么**不能**说"c1 指纹变了"

c1 检查点记录的源码指纹是 `B363F176…`，文件数 **966**。我用两种口径测：

| 口径 | 文件数 | 结论 |
| --- | --- | --- |
| 排除 `build32` **和** `build` | 933 | 与 c1 的 966 差 33 ⇒ **口径不同** |
| 只排除 `build32`（含 `src/minhook/build/**`） | 964 | 与 c1 的 966 差 **2** ⇒ **仍不同** |

两者都对不上，且我**无法确定 c1 当年用的是哪种口径**。

⇒ **`B363F176…` 与 `D829DAE2…` 之间不存在可验证的"变化"关系** —— 我没有资格说
「检查点因本轮修复而失效」，因为我连基线口径都复现不出来。
⇒ 这是**检查点①本身的缺陷**：它记录了一个**没有同时记录口径**的指纹，导致该指纹**不可复核**。

## 3. 修正做法（从现在起）

任何源码指纹**必须与其计算口径写在一起**（排除规则、排序、每行格式、是否含空文件）。
本文第 1 节给出的脚本即为当前口径的唯一权威定义；后续只说"以 §1 口径计，指纹为 X"。

## 4. 现场与回退

```
测试环境（可写、我在这里构建与取证）  : E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk-v1.22-integration-20260914
玩家安装目录（**只读、不得覆盖**）      : E:\Work\Warcraft III\d3d9.dll
  └ 当前 = 基线 F275545B…（未部署本轮候选）
回退目标                              : 同上基线；另有 rename-park 恢复工具 AutoTest/restore_site_dll.py
绝对不得触碰                          : E:\Work\War3  ← 与玩家安装目录**不是**同一目录
```

## 5. 本轮状态

```
候选 DLL 含：P0 版本门控 / P1 deltaFrames / P1 完成判据按链型 / 写方修正① + 其多帧回归锁
验证：war3_palette_object_evidence_test 24 passed 0 failed；meson 85 Ok / 0 Fail；静态 259/0
**未部署、未提交、未晋升稳定**；候选 ≠ 发布。
```