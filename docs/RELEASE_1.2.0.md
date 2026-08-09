# WarVK 1.2.0 发布范围

本文区分 GitHub 源码、玩家二进制包和地图作者包。三者不得通过 `git add -A` 或复制整个工作目录生成。

## GitHub 源码应包含

- 运行时代码：`src/`、`include/`、`shaders/`、`smaa/`。
- Shader 子项目与已固定的依赖 gitlink：`subprojects/war3fx/` 及 `.gitmodules` 中正式依赖。
- 构建入口：`meson.build`、`meson_options.txt`、`build-win32.txt`、`build-win64.txt`、
  `build32_safe.cmd`、`package-*.sh`、`version.h*`、`RELEASE`。
- 发布文档与许可证：`README.md`、`README_CN.md`、`CHANGELOG.md`、`LICENSE`、`COPYING`、
  `THIRD_PARTY_NOTICES.md` 和本文件。
- 可复现的静态/Win32 测试、分析器与正式 AutoTest conductor。
- `WarVK/` 中的 JASS 库、YDWE `action/call/define`、作者文档与 `icons/atom.blp`、
  `icons/atom.png`。
- `docs/` 中仍用于解释公开架构、逆向证据、正确性合同和维护边界的源文档。

提交前必须逐路径添加。当前独立 `StormBreaker` 工作树若含未发布实验，只保留主仓库已固定的 gitlink，
不得把本地 WIP 指针随 1.2.0 推送。

## 玩家二进制包应包含

- `d3d9.dll`（Win32 Release 构建）。
- `README_CN.md`、`README.md`、`CHANGELOG.md`。
- `LICENSE`、`COPYING`、`THIRD_PARTY_NOTICES.md`。

玩家包不需要源码、JASS/YDWE 文件、AutoTest、研究资料、调试日志、PDB/OBJ、构建目录或地图文件。
发布页应同时给出 `d3d9.dll` 的文件大小与 SHA-256。

## 地图作者包应包含

- `WarVK/action.txt`、`WarVK/call.txt`、`WarVK/define.txt`。
- `WarVK/jass/`。
- `WarVK/icons/atom.blp`、`WarVK/icons/atom.png`。
- `WarVK/README.md`、`WarVK/MATH_CURVE_API.md`。

作者包不得包含 `loader/`、`bin/`、`WarVK.dll`、伪装为 `.blp` 的 DLL carrier 或 Lua/AI
加载脚本。WarVK 仅由玩家在启动游戏前安装的代理 `d3d9.dll` 提供运行时。

## 明确不上传

- `build/`、`build32/`、`build64/`、`build_vs2022/`、`build32_debug/` 等构建目录。
- `output/`、`research_bundles/`、`AutoTest/artifacts/`、`WarVK/bin/`。
- `.cache/`、`.pytest_cache/`、`.playwright-cli/`、编辑器配置、agent 私有配置。
- `*.dll`、`*.exe`、`*.obj`、`*.o`、`*.a`、`*.lib`、`*.pdb`、`*.dmp`、`*.log`。
- 私有地图、SourceMap、音频、截图、性能报告、运行时 JSON/JSONL、驱动事件和崩溃现场。
- `docs.zip`、`docs (2).zip`、`diff_codex*.txt`、HTML 预览、一次性 `_patch_*.py`、
  `_gen_*preview*.py`、`war3_internal_test_config(R*).h` 等临时副本。
- 未经单独审查和固定的 `subprojects/StormBreaker` 本地 WIP。

## 发布前门槛

1. 所有 `test_*_static.py` 通过。
2. 所有 build32 中的 WarVK Win32 runnable 返回 0。
3. `build32_safe.cmd src/d3d9/d3d9.dll -j8` 通过，随后 `ninja -C build32 -n` 为 no-work。
4. WarVK YDWE Catalog 由真实 `UiCatalog::Load` 解析并通过 WTG/WCT 校验。
5. `git diff --check` 无新增 whitespace error；版本一致性测试通过。
6. 人工检查待提交路径，不包含上节排除项；不使用 `git add -A`。
7. 记录最终 DLL 文件大小、SHA-256、回退文件，并在用户确认后再创建 `v1.2.0` 标签/发布。

## 当前验收边界

静态测试、Win32 runnable 和构建只证明代码与合同闭合，不替代物理前台验收。1.2.0 的发布门
限定为“新启动进程只进入一张地图”的常见平台流程；同进程跨地图 A→B→A 已确认为已知问题并
延期修复，不再作为本版本的发布承诺。1.2003 的建造附件阴影与点阴影 receiver-bias 修复已通过
玩家前台验收；后续运行时安全组合通过离线合同和 clean build，但仍不能替代 Reset/A→B→A 物理门。
