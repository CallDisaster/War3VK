# v1.22.02 发布事务回执

## 远程结果

2026-11-11T15:23:09Z（台北 23:23）正式公开：Release ID 441866256，draft=false、prerelease=false、
latest=v1.22.02。[下载页](https://github.com/CallDisaster/War3VK/releases/tag/v1.22.02)。

- 标签 `v1.22.02` 是附注标签，标签对象为 c90c88800fccabf5cbe9e99475a1159bea481f27，指向
  a5717bf971a1e96e180d166354ad228b64a0dbd3。
- main 从 247a983 快进到 a5717bf，与标签一起用 `--atomic` 推送，没有 force。
- 旧标签和旧版本附件均未改动；v1.22.01 的四个附件 digest 复核不变。
- 推送范围共 5 个提交、569 个文件（+105,577/−2,335）。提交身份统一为 qilin；不含二进制文件和凭据。

| 附件 | 字节 | SHA-256 |
| --- | ---: | --- |
| d3d9.dll | 21,502,485 | `587D29429F575094377E4298E793E09A6F54202BBD711D27F46B320F079672A5` |
| WarVK-1.22.02-win32.zip | 7,408,449 | `067741ADD295D223C45ED35DDD07141DBC4A5FF7EE34588C0CD71E3042BFCD80` |
| WarVK-1.22.02-author-kit.zip | 107,689 | `3047DA484D9B572D9243BAB5810A7BE2C2E4BFCFB8C3E99AB428007B286EFDFD` |
| SHA256SUMS.txt | 260 | `C736AD2C57AA99469579F0B05B0CB47035EA4488FA8A8F6704A790F97F47CBBE` |

先建草稿。草稿阶段核对了两项：远端四个附件的 digest 与本地一致，正文与
`docs/RELEASE_NOTES_1.22.02.md` 逐字相同。核对通过后才公开，公开后再次读取 latest 复核。

## 本地核验

- 打包命令：`package_v122_release.py --manifest source-final-3.json`，输出到
  `E:/WarVK-Releases/1.22.02-20261111`。`release-audit.json` 为 ok=true，sourceCommit 为 a5717bf。
- 发布 DLL 是 `strip --strip-unneeded --preserve-dates` 生成的副本。对照做过实机门的未剥离
  `25A220AE…7A2C`：
  - 入口、基址、时间戳相同；
  - 8 个可加载段（.text .data .rdata .bss .edata .idata .rsrc .tls）逐字节相同；
  - 导入/导出表一致（打包器检查）；
  - 只有 SizeOfImage 因去掉调试段，从 80,642,048 变为 80,465,920。
- ZIP 检查：
  - 两个 ZIP 的 testzip 都通过；
  - 玩家包 12 个成员，包内 `d3d9.dll` 与独立附件 SHA 相同；
  - 作者包 18 个成员；
  - SHA256SUMS 的三行与文件一致。

## 现场与边界

- 玩家目录 `d3d9.dll` 在发布前后都复核为 `1FFE0BD1…35CF`，本次没有部署。
- StormBreaker 用户工作树的未提交修改保留，没有进入构建或提交。
- 发布范围、验证表与未验证项见 [RELEASE_1.22.02](../RELEASE_1.22.02.md)。
- 本回执在打标签之后单独提交到 main，不移动标签，也不重新打包。
