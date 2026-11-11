# AutoTest 授权沙盒根修正

记录时间：台北 2026-10-06 02:02。仅修改工具的默认专用沙盒根，没有修改任何作者游戏安装、YDWE、注册表、地图原件或启动/隔离逻辑。

作者要求 18.4 只用 `E:/Work/War3_AutoTestSandbox`，并允许必须修改 AutoTest 时在 War3MapReforge 单独记录和报告。主代理检查实际门禁失败后，将此单行修正交给 18.4 专用代理执行。

## Git 与文件身份

- 实际代码所属 Git 根：`E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk`。
- 修改时该仓库 HEAD：`a2054a9776e40edc3f01f6f390c06037965b8ef2`。
- 上级 War3MapReforge HEAD：`285cb81c39b003deeb636f872246af63b1e38ffe`。
- 目标文件 `AutoTest/autotest_sessions.py` 在修改前无 tracked dirty diff。没有重置或清除仓库其他文件。
- 修改前文件 SHA-256：`43bf354bbb4582c3833f3638f069e3ff5e8b0233f9d549e27a38e8f9632787dd`。
- 修改后文件 SHA-256：`d1bfb33604ac924e6690e27d82afeea978df384633f8af32ce1423064e2c7046`。
- `git diff --numstat -- AutoTest/autotest_sessions.py`：1 增、1 删。未 commit / push。

第 25 行由：

```python
DEFAULT_SANDBOX_ROOT = Path(r"E:\Work\Warcraft III")
```

改为：

```python
DEFAULT_SANDBOX_ROOT = Path(r"E:\Work\War3_AutoTestSandbox")
```

仍为唯一精确路径根；没有添加第二根、放宽 containment、篡改进程内存常量或跳过拒绝。

## 原因与验证

修正前，真实公开 `preflight_instance_pool(sandbox_root=E:/Work/War3_AutoTestSandbox, instance_count=1)` 返回 `ok=false`，原因是只允许 `E:/Work/Warcraft III`。这是工具默认值与 README/作者授权不一致，不是地图问题。

修正后从新 MCP stdio 进程重新取 schema，并实际调用同一只读预检：`ok=true`、`sandboxPolicy=exact-path-only`、`allowedSandboxRoot=E:/Work/War3_AutoTestSandbox`、errors/warnings 空。会话盘点为 0。预检没有创建 `_AutoTestInstances` 或启动游戏。

纯 Python 路径、只读预检、会话隔离、部署、回收单元测试：`python -B -m unittest test_autotest_sessions.py`，12/12，0.808 秒，实际退出码 0。这些是工具单元测试，不是 Warcraft 进图或性能验收。

现有全局 Python 的 MCP 2.x 不提供 FastMCP；仅在编辑器本轮 `nightly-maptest-20261006/mcp1-runtime` 准备 PyPI 缓存 wheel `mcp 1.30.0`（`--target --no-deps`），不改全局 Python、AutoTest 配置或产品包。

公开 schema、实际预检和后续地图运行证据统一保存在：

`D:/Work/War3MapEditor64-p8-20261005/artifacts/astra-rounds/nightly-maptest-20261006/`

其中 `mcp-public-schema.local.json` 为实际工具 schema，`mcp-readonly-audit.local.json` 为修正后实际预检与会话结果，`REPORT.md` 记录每张地图的分类和运行证据。

## 嵌套仓库内的独立记录

本记录原先已保存在 War3MapReforge 的 `artifacts/astra-rounds/nightly-maptest-20261006/AUTOTEST_ROOT_FIX.md`；本轮按主代理授权，将这一个小报告另放入实际代码所属的 AutoTest 目录，方便关联仓库独立审查。不复制地图、游戏、项目或构建产物。

补文档前实际嵌套仓库 HEAD 仍为 `a2054a9776e40edc3f01f6f390c06037965b8ef2`。该仓库有多处已有 tracked/untracked 修改，包括渲染、AutoTest 分析器和他人测试；本轮没有 stage、commit、reset、清理或纳入它们。`autotest_sessions.py` 当前 diff 仍只包含上述 1 增/1 删，实际 SHA 仍为 `d1bfb33604ac924e6690e27d82afeea978df384633f8af32ce1423064e2c7046`。

本轮在这个嵌套仓库的范围仅为该单行修正和本新增报告；未部署任何 DLL，未改变视频/注册表/隔离逻辑，未合 main、未 push。本文的 12 个单元测试和公开 MCP 只读预检是当时已捕获的实际回执，本次补文档没有重跑游戏或把它们描述为进图通过。
