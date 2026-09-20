# ✅ 检查点可还原性首次验证 + 指纹定义修正 — 2026-09-18

> 目标①要求"**完整可还原**的本地检查点"。此前我只**记录**了身份，**从未验证过归档能否还原**。本轮补上，结果同时暴露了我指纹定义的缺陷。

## 1. 归档可还原性：**通过**

```
Expand-Archive  E:\Work\WarVK-checkpoint-c1-20260918.zip
必需件齐全：meson.build / meson_options.txt / src / AutoTest / docs / meson-info   全部 True
归档内 meson-info/intro-buildoptions.json 记录的实际构建选项：
    warvk_skin_palette_contract_candidate = True
    buildtype                             = release
    warning_level                         = 2
    b_ndebug                              = false
    ⇒ 与我在文档里声称的**完全一致**（不是转述，是归档自身的记录）
内容与场景 G 同代：ScenarioG / check_scenario_g / PALETTE_OBJECT_FIRST_SIGHT_VERSION=3  全部 True
```

**逐文件指纹比对（同一方法对两边）**：

```
repo    : n=966  fp=B363F1769E9EF2514E5DD7616FB27C0C93EE23A2318B843BF26B2F71ED010EE2
archive : n=966  fp=B363F1769E9EF2514E5DD7616FB27C0C93EE23A2318B843BF26B2F71ED010EE2
MATCH = True
```

⇒ **归档是仓库 `src/` 的忠实快照**（966 个文件逐一同哈希）。

## 2. 修正：我记录的指纹是错的

| | 说明 |
| --- | --- |
| 旧定义 | `Get-ChildItem src -Recurse -File | Where-Object { $_.FullName -notmatch '\\build' }` |
| 缺陷 | 该过滤把所有路径含 `\build\` 的文件排除，但 **`src/minhook/build/**` 是 MinHook 的源码构建脚本**（`Makefile`/`make.bat`/`.vcxproj`…），**不是构建产物** |
| 后果 | 指纹建立在**966 → 932** 的任意子集上；绝对值无意义（内部一致，但对外不可复现） |
| 旧值（错误） | `57D917F9…`（c1 初版）→ `064B316A…`（场景 G 后），均为 **932 文件** |
| **正确值** | **`B363F1769E9EF2514E5DD7616FB27C0C93EE23A2318B843BF26B2F71ED010EE2`（966 文件）** |

**新定义**（两边统一、可复现）：以 `src` 目录自身为基准取相对路径 ——

```powershell
function Fingerprint([string]$srcDir) {
  $src = (Get-Item $srcDir).FullName          # 必须解析，避免 8.3 短名导致前缀对不齐
  $files = Get-ChildItem $src -Recurse -File  # 不过滤：minhook/build 是源码
  $concat = ($files | Sort-Object FullName | ForEach-Object {
      $_.FullName.Substring($src.Length) + '|' + (Get-FileHash $_.FullName -Algorithm SHA256).Hash }) -join "`n"
  ... SHA256(UTF8($concat))
}
```

## 3. 方法论上的第二个坑

第一次比对时我得到"不匹配"，一度以为归档损坏。**实际是比对方法坏了**：
`Expand-Archive` 的 `$ex` 是 **8.3 短名**（`...\ADMINI~1\...`），而 `$_.FullName` 是**长名**，
于是 `Substring($ex.Length)` 从长名里砍掉的是**错的字符数**，两边路径前缀不同 ⇒ 同内容也算出不同指纹。
输出里的 `rify2\src\...` 就是这个 bug 的可见痕迹。

⇒ 教训（第五条，与前四条同源）：**比对失败时，先怀疑比对，再怀疑被比对的东西。**
前四条：①只读部分输出；②把"机制看起来对"当成已验证；③把"符号存在"当成端到端可用；④把"无法观测"当成"观测到否定"。

## 4. 目标①的完成度（更新后）

| 要求 | 状态 |
| --- | --- |
| 含源码、根构建配置、生成器、AutoTest、Shader、实际构建选项 | ✅ 归档内含，且构建选项由归档自身 `meson-info` 佐证 |
| **完整可还原** | **✅ 本轮首次实证**（966 文件逐一同哈希） |
| 记录"测的是哪份源码/哪个配置/回退回到哪里" | ✅ 指纹已修正为可复现值；构建选项已核对 |
| 显式区分测试环境与玩家安装目录 | ✅ `…\build32` vs `E:\Work\Warcraft III`；另注明 `E:\Work\War3` 不得触碰 |

## 5. 仍未闭合

- **仍无实机 v3 导出**；第④问第四子问题缺运行期证据；
- 阻塞：`War3.exe` pid 18948 仍存活（`taskkill /F /PID 18948` 可解）⇒ 采集运行无法启动；
- 现场 DLL = 基线 `F275545B…5CF07FF3`，未触碰；未提交、未部署。