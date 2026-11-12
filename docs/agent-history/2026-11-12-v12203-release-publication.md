# v1.22.03 发布事务回执

## 远程结果

2026-11-12T07:09:53Z（台北 15:09）正式公开：Release ID 442599813，draft=false、prerelease=false、
latest=v1.22.03。[下载页](https://github.com/CallDisaster/War3VK/releases/tag/v1.22.03)。

- 标签 `v1.22.03` 是附注标签，标签对象为 1be15ea8b6cd9ef492ab500a241e318c2613b3e6，指向
  c224a8c86de96e0cc45a5d29c204ee5110caad36。
- main 从 c5b5774 快进到 c224a8c，与标签一起用 `--atomic` 推送，没有 force。
- 旧标签和旧版本附件均未改动：v1.22.02 标签对象仍为 c90c888（指向 a5717bf），四个附件 digest 复核不变。
- 推送范围共 3 个提交、45 个文件（+1,803/−197）。提交身份统一为 qilin；不含二进制文件和凭据。

| 附件 | 字节 | SHA-256 |
| --- | ---: | --- |
| d3d9.dll | 21,572,117 | `7795BF90E0869CDA318E361A747DFEA07067AE0BEA5EF9B7CABF0BD3FFA571FB` |
| WarVK-1.22.03-win32.zip | 7,443,971 | `D17CDE2A60093E031BBB0B652B0D9DC309701F523FD1DFD11F9AE8A7BA8C0032` |
| WarVK-1.22.03-author-kit.zip | 107,691 | `F8AC8A80575A6C1C01511DFE11B6B9DACF835CBD6D99AC25E5D2BBD75AEC3651` |
| SHA256SUMS.txt | 260 | `E7ACBEF904395395018000286744E35CBDDEC4C87FB2BD3057986681E9C8E7E8` |

先建草稿。草稿阶段核对了两项：远端四个附件的 digest 与本地一致，正文与
`docs/RELEASE_NOTES_1.22.03.md` 逐字相同（只把 CRLF 换成 LF）。核对通过后才公开，公开后再次读取
latest 复核。

## 本地核验

- 打包命令：`package_v122_release.py --manifest source-final.json`，输出到
  `E:/WarVK-Releases/1.22.03-20261112`。`release-audit.json` 为 ok=true，sourceCommit 为 c224a8c，
  meson 122、静态 287。
- `source-final.json` 相对首次冻结（87ea383）只补入三份文档：`docs/RELEASE_1.22.03.md`、
  `docs/RELEASE_NOTES_1.22.03.md`、`docs/agent-history/DEVELOPMENT_CHANGELOG.md`；运行时源码与构建
  输入不变，打包器逐一核对了 `src/d3d9`、`src/dxvk`、`src/util`、`subprojects/war3fx` 与仓库一致。
- 发布 DLL 是 `strip --strip-unneeded --preserve-dates` 生成的副本。对照做过实机门的未剥离
  `54C4642E…AD73`：可加载段的名称与大小完全相同，只少了 9 个 `.debug_*` 段；导入/导出表一致
  （打包器检查）；版本资源 1.22.03、非预发布。
- ZIP 检查：
  - 两个 ZIP 的 testzip 都通过；
  - 玩家包 12 个成员，包内 `d3d9.dll` 与独立附件 SHA 相同，包内 README 为 1.22.03；
  - 作者包 18 个成员；
  - SHA256SUMS 的三行与文件一致。

## 现场与边界

- 玩家目录 `d3d9.dll` 在每个门前后与发布后都复核为 `1FFE0BD1…35CF`，本次没有部署。
- StormBreaker 用户工作树的未提交修改保留，没有进入构建或提交。
- 本轮途中远程桌面会话断开过一次，期间换图门被测试工具拒绝启动（未启动游戏）；回执
  `gate-crossmap-r1-session-disconnected.log` 保留在构建目录，会话恢复后重跑通过。
- 发布范围、根因数据、验证表与未验证项见 [RELEASE_1.22.03](../RELEASE_1.22.03.md)。
- 本回执在打标签之后单独提交到 main，不移动标签，也不重新打包。
