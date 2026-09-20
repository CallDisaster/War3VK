# ✅ 检查点 c2：**还原性验证通过** — 2026-09-18

## 1. 结论

```
zip  src fp : 964 files  D829DAE2828BE6DEA0A6D7F8D9C64DCC10B42531A980B35B879D1DDB349A08A5
repo src fp : 964 files  D829DAE2828BE6DEA0A6D7F8D9C64DCC10B42531A980B35B879D1DDB349A08A5
MATCH       : ✅ 一致
```

⇒ **c2 归档可以还原出与仓库逐字节一致的 `src`** ⇒ **目标①的"可还原"部分闭环**。

## 2. 上一轮的 `MATCH: False` 确认是我的测量误差

round 228 我用 `$_.FullName.Substring($base.Length+1)` 求相对路径，而 `$env:TEMP` 形如
`D:\tmp\AppData\ADMINI~1\Local\Temp\...`（**含 8.3 短名**），`Get-ChildItem` 返回**长名**形式
⇒ 偏移量不符 ⇒ 相对路径整体错位 ⇒ 哈希必然不同而**文件数不变**（964 vs 964）。

**本轮的修正**：

| 改动 | 作用 |
| --- | --- |
| 解压到 `E:\Work\_ckpt\c2ex`（**无 8.3 短名**） | 消除短/长名不一致 |
| 用 `(Get-Item $b).FullName` **规范化 base** | 两侧同一形式 |
| `Substring($nb.Length).TrimStart('\\')` | 不依赖 `+1` 偏移 |

⇒ **c2 实际一直是完好的**，是我的校验脚本错了两次（round 228 的偏移、round 229 的单引号不插值）。

## 3. c2 归档定义（**这是本次真正补上的东西**）

c1 的教训是"记录了指纹却没记录口径 ⇒ 不可复核"。c2 把**选择规则与排除规则一并写下**：

```
归档路径 : E:\Work\WarVK-checkpoint-c2-20260918.zip
大小     : 26.62 MB
SHA-256  : 0DDE7D4E229B0298663CA5DAF2BFF33EC966A03958ED6A61FEBE8C26856B7992

包含（目录）  : src, shaders, include, smaa, AutoTest, tools, WarVK
包含（根文件）: meson.build, meson_options.txt, build32_safe.cmd,
                build-win32.txt, build-win64.txt, AGENTS.md
包含（构建选项）: build-options/intro-buildoptions.json（= build32/meson-info/ 的副本）

**必须排除**  : AutoTest/artifacts/     <- 占 31,974 MB（运行产物：frame_evidence_runs 20.2 GB 等）
                 build32/               <- 构建输出，不入档
                 src/**/build32/**      <- 同上
不包含        : subprojects/            <- 外部依赖，按 meson wrap 复原
```

## 4. 源码指纹口径（**唯一权威定义**）

```powershell
function Fp([string]$b) {
  $nb = (Get-Item $b).FullName                      # 规范化 base（关键）
  $s = Get-ChildItem (Join-Path $nb 'src') -Recurse -File -Force |
         Where-Object { $_.FullName -notmatch 'build32' -and $_.FullName -notmatch '\.git' }
  $l = $s | Sort-Object FullName | ForEach-Object {
         $rel = $_.FullName.Substring($nb.Length).TrimStart('\')
         "$rel|$($_.Length)|$((Get-FileHash $_.FullName -Algorithm SHA256).Hash)" }
  $h = sha256(($l -join "`n"))
}
```

**规则**：只含 `src/`；排除路径含 `build32` 或 `.git` 的文件；按 `FullName` 排序；
每行 `相对路径|字节数|SHA256`；以 `\n` 连接；UTF-8 编码；再取 SHA-256。

当前值：**964 文件 / `D829DAE2828BE6DEA0A6D7F8D9C64DCC10B42531A980B35B879D1DDB349A08A5`**

## 5. 这个检查点"测的是哪份源码/哪个配置"

| 问题 | 答案 |
| --- | --- |
| 哪份源码 | 上列指纹（964 文件）的那份 |
| 哪个配置 | `warvk_skin_palette_contract_candidate=True`、`warvk_internal_frame_recorder=True`、`buildtype=release`、`warning_level=2`、`b_ndebug=false`（见 `build-options/intro-buildoptions.json`） |
| 回退回到哪里 | 现场站点 `E:\Work\Warcraft III\d3d9.dll` = 基线 `F275545BAA65A0153804AC1CC0B3533B595DA24B5725488B8FFB9DDC5CF07FF3`；恢复工具 `AutoTest/restore_site_dll.py`（rename-park） |
| 测试环境 | 仓库树（可写，构建与取证都在此） |
| 玩家安装目录 | `E:\Work\Warcraft III\d3d9.dll`（**只读；本轮候选未部署**） |
| **绝对不得触碰** | `E:\Work\War3`（**与玩家安装目录不是同一目录**） |

## 6. 仍**未**闭环的部分（说清）

| 项 | 状态 |
| --- | --- |
| 源码/配置/回退/环境区分 | ✅ 闭环 |
| 归档可还原（`src` 逐字节一致） | ✅ 闭环 |
| 归档中 `AutoTest/`、`shaders/`、根配置的**逐字节**还原性 | ⚠️ **未单独校验**（只校验了 `src`） |
| "检查点 ≠ 发布" | ✅ 本文档与进度日志均明确 |

## 7. 现场

```
候选 DLL 含：P0 版本门控 / P1 deltaFrames / P1 完成判据按链型 / 写方修正① + 多帧回归锁
验证：war3_palette_object_evidence_test 24 passed 0 failed；meson 85 Ok / 0 Fail；静态 259/0
未提交、未部署、未晋升稳定。
```
---

## 8. ✅ 补充验证：`AutoTest` / `shaders` / 根配置也逐字节可还原（2026-09-18 追加）

上一节把"归档中 `AutoTest/`、`shaders/`、根配置的逐字节还原性"列为**未校验**。现已补齐：

```
repo: 700 files  FCC6B3425EA69644307F3A77D423B132BEFA69613C3B09D5565D636A0CD74D01
zip : 700 files  FCC6B3425EA69644307F3A77D423B132BEFA69613C3B09D5565D636A0CD74D01
MATCH(AutoTest+shaders+root): True
```

口径：`AutoTest` + `shaders` 递归全部文件（**排除 `artifacts`**，与归档排除规则一致）+ 根文件
（`meson.build`/`meson_options.txt`/`build32_safe.cmd`/`build-win32.txt`/`build-win64.txt`/`AGENTS.md`）；
规范化的 base、排序、每行 `相对路径|字节数|SHA256`、`\n` 连接。

### 目标① 的完整闭环清单

| 要求 | 证据 | 状态 |
| --- | --- | --- |
| 源码 | 964 文件指纹，解档一致 | ✅ |
| 根构建配置 | 含在上表 700 文件内 | ✅ |
| 生成器 | `build32_safe.cmd` 含在内 | ✅ |
| AutoTest | 700 文件指纹，解档一致 | ✅ |
| Shader | 700 文件指纹（`shaders/`），解档一致 | ✅ |
| 实际构建选项 | `build-options/intro-buildoptions.json` | ✅ |
| 「测的是哪份源码/哪个配置」 | §5 表格 | ✅ |
| 「回退回到哪里」 | §5：站点基线 + `restore_site_dll.py` | ✅ |
| 测试环境 vs 玩家安装目录 | §5：仓库树 vs `E:\Work\Warcraft III`；**不得触碰 `E:\Work\War3`** | ✅ |
| 检查点 ≠ 发布 | 本文 §7 与进度日志均明确 | ✅ |
