# WarVK 1.21.00 发布范围

本文区分 GitHub 源码、玩家二进制包和地图作者包。三者必须按白名单生成，禁止使用 `git add -A`
或复制整个工作目录。

## GitHub 源码应包含

- 运行时代码：`src/`、`include/`、`shaders/`、`smaa/`。
- Shader 子项目与已固定依赖 gitlink：`subprojects/war3fx/` 及正式 `.gitmodules` 项。
- 构建入口：`meson.build`、`meson_options.txt`、cross files、`build32_safe.cmd`、打包脚本、
  `version.h*` 与 `RELEASE`。
- 发布文档与许可证：`README.md`、`README_CN.md`、`CHANGELOG.md`、`LICENSE`、`COPYING`、
  `THIRD_PARTY_NOTICES.md`、本范围文件及 `RELEASE_NOTES_1.21.00.md`。
- 可复现的静态/Win32 测试、正式 AutoTest conductor 和运行时诊断分析器。
- `WarVK/` 中的 JASS 库、YDWE `action/call/define`、作者文档与正式图标。
- 仍用于解释公开架构、正确性合同和维护边界的 `docs/` 源文档。

未经单独审查的外部子模块 WIP、用户地图、日志和构建产物不得进入源码提交。

## 玩家二进制包

建议资产名：`WarVK-1.21.00-win32.zip`。

只包含：

- `d3d9.dll`（Win32 Release 构建）；
- `README_CN.md`、`README.md`、`CHANGELOG.md`；
- `LICENSE`、`COPYING`、`THIRD_PARTY_NOTICES.md`。

发布页必须同时给出 DLL 文件大小和 SHA-256。玩家包不包含源码、JASS/YDWE、AutoTest、PDB/OBJ、
研究资料、地图、日志或 DLL Loader。

## 地图作者包

建议资产名：`WarVK-1.21.00-map-author.zip`。

只包含：

- `WarVK/action.txt`、`WarVK/call.txt`、`WarVK/define.txt`；
- `WarVK/jass/`；
- `WarVK/icons/atom.blp`、`WarVK/icons/atom.png`；
- `WarVK/README.md`、`WarVK/MATH_CURVE_API.md`。

作者包不得包含 `loader/`、`bin/`、`WarVK.dll`、DLL carrier 或 Lua/AI 加载脚本。WarVK 运行时只由
玩家在游戏启动前安装的代理 `d3d9.dll` 提供。

## 明确排除

- 所有 `build*`、`output/`、`research_bundles/`、`AutoTest/artifacts/`、`WarVK/bin/`。
- cache、编辑器配置、agent 私有配置、一次性脚本、HTML 性能报告和临时补丁副本。
- `*.dll`、`*.exe`、`*.obj`、`*.o`、`*.a`、`*.lib`、`*.pdb`、`*.dmp`、`*.log`；仅 GitHub
  Release 白名单打包阶段允许将最终 `d3d9.dll` 放入外部资产目录。
- 私有地图、SourceMap、截图、运行时 JSON/JSONL、Windows GPU 事件和崩溃现场。
- 未经固定和审查的 `StormBreaker` 或其他外部工作树内容。
- 任何名为 Loader、运行时下载器或伪装资源的 DLL 注入方案。

## 发布前门槛

1. 版本一致性合同证明 Meson、`RELEASE`、`version.h`、DLL resource、JAPI 文本和 README 均为
   `1.21.00`，同时 Shader API `1.2.0` 与 `warvk:v1` 不变。
2. 全部相关 `test_*_static.py` 与全部 Win32 WarVK runnable 通过。
3. 在全新短路径目录执行 Win32 Release `-j2` 构建，随后 exact DLL target `ninja -n` 为 no-work。
4. 真实 YDWE Catalog 回读、WTG/WCT 校验通过；作者包文件清单与 Loader 排除合同通过。
5. `git diff --check` 无新增 whitespace error；人工逐路径暂存，不使用 `git add -A`。
6. 记录最终 DLL 大小、SHA-256、回退 DLL，以及玩家/作者两个 zip 的 SHA-256。
7. 用户审核 README、CHANGELOG、Release Notes 与候选包后，才允许合并/推送 main、创建
   `v1.21.00` 标签并上传 GitHub Release。

## 当前验收边界

- 已有用户前台确认建造动画、点阴影及当前体积表现基本正常；生产者 CPU 收益有同场景 A-B-B-A。
- 隔离桌面 smoke 和“生与死”低视角长门证明当前候选稳定，但不替代玩家前台绝对 FPS。
- 同进程跨地图、Issue #5 提前剔除 Consume 与 4K Froxel 最坏请求不作为 1.21.00 完成承诺；
  README 与 Release Notes 必须保留这些边界。
- 本文只授权准备本地审核候选，不授权推送 GitHub、创建标签或发布资产。
