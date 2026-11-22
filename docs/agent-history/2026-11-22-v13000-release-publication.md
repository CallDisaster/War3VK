# v1.30.00 发布事务回执

## 远程结果

2026-11-22T03:00:07Z（台北 11:00）正式公开：Release ID 453934178，draft=false、prerelease=false、
latest=v1.30.00。[下载页](https://github.com/CallDisaster/War3VK/releases/tag/v1.30.00)。用户确认原话：“推送并发布”。

- 标签 `v1.30.00` 是附注标签，标签对象为 6b2ec4a91fed8ed85f63723e6456ab7d8f277984，指向
  01742a4f206aee2a248697b1860a8e821c4aba22。
- main 从 a7e1cba 快进到 01742a4，与标签一起用 `--atomic` 推送，没有 force。本地工作分支是
  `codex/v1.22-main-tree-20260920`，所以推送用显式 refspec `01742a4…:refs/heads/main`。
- 旧标签和旧版本附件均未改动：v1.22.03 标签对象仍为 1be15ea（指向 c224a8c），v1.22.02 仍为 c90c888
  （指向 a5717bf）；v1.22.03 四个附件 digest 复核不变。
- 推送范围共 3 个提交（2d619f4、a8be8a0、01742a4）、340 个文件（+57,468/−5,238）。提交身份统一为 qilin；
  不含二进制文件和凭据；StormBreaker gitlink 没有提交。

| 附件 | 字节 | SHA-256 |
| --- | ---: | --- |
| d3d9.dll | 22,227,959 | `0976B36672D019DCA3BF24D04622C01D65A481B45B70A6A7C48C1466E99FBEED` |
| WarVK-1.30.00-win32.zip | 10,820,670 | `561FBCCA06B8EA81243FF3B4B106B5E8872360FA5679D60313BC474747DDC8E9` |
| WarVK-1.30.00-author-kit.zip | 108,481 | `48923428BFF95A66D5C3CA8AD2CB93B6E258341F2AFFB160676A8CF25777414F` |
| SHA256SUMS.txt | 260 | `6B1B523305A272E7662DA47E134F24FA43A9038052D0378D247905F610875B9E` |

先建草稿（当时标签还没推送，草稿以 `main` 为目标、标签名 v1.30.00）。草稿阶段核对了两项：远端四个附件的
digest 与本地一致，正文与 `docs/RELEASE_NOTES_1.30.00.md` 逐字相同（只把 CRLF 换成 LF）。用户确认后先推送，
再次读取草稿附件，然后公开；公开后读取 latest 复核附件 digest 与正文仍一致，远端标签 ref 指向 6b2ec4a。

## 本地核验

- 打包命令：`package_v122_release.py --manifest source-final.json`（前端为 11-18 冻结的 f8，宿主回执为冻结
  源码里的 `AutoTest/artifacts/host-build-release-r1/receipt.json`），输出到 `E:/WarVK-Releases/1.30.00-20261122`。
  `release-audit.json` 为 ok=true，sourceCommit 为 01742a4，meson 122、静态 300。
- 冻结清单链：`source-manifest.json`（a8be8a0 首次冻结，4544 个文件）→ `source-tools.json`（对照门、探针、
  1.30 静态测试、README/CHANGELOG、开发记录与计划）→ `source-tools-r2.json`（三个静态测试的期望）→
  `source-final.json`（两份发布文档与开发记录，4546 个文件）。运行时源码与构建输入始终未变，打包器逐一核对了
  `src/d3d9`、`src/dxvk`、`src/util`、`subprojects/war3fx`、`tools/render_host` 与仓库一致。
- 发布 DLL 是 `strip --strip-unneeded --preserve-dates` 生成的副本，对照做过实机门的未剥离
  `EC87666C…E778`：导入/导出表一致（打包器检查）；版本资源 1.30.00、非预发布。
- 64 位文件用 llvm-strip 剥离：前端 1,257,472 → 897,024 B（`13EE141C…3967`），宿主 8,232,960 → 7,573,504 B
  （`FF825678…7C62`），两者只依赖系统 DLL（打包器检查导入表）。
- ZIP 检查：两个 ZIP 的 testzip 都通过；玩家包 16 个成员（含 `WarVK/x64` 的前端、宿主和 `dxvk.conf`），包内
  `d3d9.dll` 与独立附件 SHA 相同，`WarVK_EvidenceStart.cmd` 为纯 ASCII；作者包 18 个成员；SHA256SUMS 的三行与
  文件一致。

## 现场与边界

- 作者本机游戏目录的 `d3d9.dll`（作者 11-18 的试用文件，21,594,112 B，`D504047A…C303`）在每个门前后与
  发布后都复核不变，本次没有部署。
- StormBreaker 用户工作树的未提交修改保留，没有进入构建或提交。
- 失败回执都留在构建目录：第一轮实机门 `gate-*-r1-build-conflict.log`（测试构建的 ninja 仍在运行，门禁拒绝
  启动）；对照门 r1–r3 的日志与 r4 的半截产物；去上限检查 r1–r11；冻结静态首轮
  `static-release-r1-stale-tests`（297/300）。
- 发布范围、性能数据、验证表与未验证项见 [RELEASE_1.30.00](../RELEASE_1.30.00.md)。
- 本回执在打标签之后单独提交到 main，不移动标签，也不重新打包。
