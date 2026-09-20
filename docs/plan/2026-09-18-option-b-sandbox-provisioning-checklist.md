# 选项 (B) 沙箱供给清单（2026-09-18 主线程实测得出）

> 用途：一旦你授权，供给 sandbox 就是一个**可照做的一次性动作**。本文件是**清单**，**未执行任何复制**。
> 背景：① T1 与 ② T8 都卡在"能进图"；自动路线要求 `launch_war3_instance(sandbox_root=…)` 的 sandbox 内**有游戏安装**，且 T8 需要特定的两张图。

## 1. 沙箱现状（实测）

`E:\Work\War3_AutoTestSandbox\` 顶层只有 4 个目录：`Maps`、`Temp`、`WarVKJapiCandidates`、`WaterObserve`。

| 期望的游戏文件 | 是否存在 |
| --- | --- |
| `war3.exe` | **False** |
| `Game.dll` | **False** |
| `d3d9.dll` | **False** |
| `Frozen Throne.exe` | **False** |

`Maps\` 下**只有** `光影测试.w3x`（**22,716 B**）。

## 2. T8 脚本的确切期望（`AutoTest/dual_perf_baseline.py:6-7`）

```
HIGH_MAP = <sandbox>/Maps/ShadowTest/光影测试(高压).w3x   # 高压段
LOW_MAP  = <sandbox>/Maps/ShadowTest/光影测试.w3x        # 低压段
```

⇒ 沙箱**缺** `Maps/ShadowTest/` 子目录，也**缺**高压那张图。

## 3. 本机已有的可用来源（实测，只读）

| 需要的目标 | 可用的本机来源 | 字节 |
| --- | --- | --- |
| `Maps/ShadowTest/光影测试.w3x`（**低压，真实大图**） | `E:\Work\War3\Maps\ShadowTest\光影测试.w3x` | **5,859,935** |
| `Maps/ShadowTest/光影测试(高压).w3x` | `E:\Work\War3\Maps\ShadowTest\光影测试(高压).w3x` | **22,716** |
| （同上，另一处副本） | `E:\Work\Warcraft III\Maps\ShadowTest\光影测试(高压).w3x` | 22,716 |
| （可选）`光影测试(桥斜坡).w3x` | `E:\Work\War3\Maps\ShadowTest\光影测试(桥斜坡).w3x` | 5,842,263 |

> **注意（重要）**：沙箱现有的 `Maps\光影测试.w3x` 是 **22,716 B**，而 T8 的低压图应当是 **5,859,935 B** 那张 ⇒ **不能**直接把现有沙箱那张改名当低压图用。
> 另注：T1 用的高压图此前一直用 `Maps\(4)生与死v1.28读档bug修复.w3x`（62 MB，在玩家目录/副本里），与 T8 的"光影测试"系列**不是同一张**；① 与 ② 的图需求应分别按各自脚本/配方选。

## 4. 授权后的一次性动作（我不会在你授权前执行）

1. **供给游戏**：把一份可运行的 Warcraft III 1.27a 安装（`war3.exe` / `Game.dll` / `Frozen Throne.exe` / 必要 DLL 与数据文件，约 0.5–1 GB）放入 `E:\Work\War3_AutoTestSandbox\` 顶层；
   - **只放进沙箱，绝不触碰 `E:\Work\Warcraft III\` 的现场文件**；
2. **供给地图**：新建 `E:\Work\War3_AutoTestSandbox\Maps\ShadowTest\`，把 §3 表里的两张图分别复制为 T8 期望的确切文件名；
3. **部署候选**：`launch_war3_instance(..., deploy_d3d9_before_launch=True, build_d3d9_path="build32/src/d3d9/d3d9.dll")` 会把**仓库候选**部署到沙箱实例内（仍不碰现场 DLL）；
4. **注入子门**：`env_overrides_json` 传 `{"DXVK_WAR3_FRAME_EVIDENCE":"1","DXVK_WAR3_FRAME_EVIDENCE_PALETTE_OBJECT":"1"}`；
5. **执行**：① 走 `2026-09-18-pre-u5-baseline-collection-protocol.md` 的两段采样；② 走 `py AutoTest/dual_perf_baseline.py`（判成功看 `ok=True` 与指标，**不看退出码**）。

## 5. 磁盘与时间估计

- 沙箱所在卷 `E:` 在本夜初实测约 **88.4 GB** 空闲 ⇒ 供给游戏（~1 GB）+ 两张图（~6 MB）余量充足；
- 复制耗时取决于来源，主要是游戏本体（建议用同卷复制以走硬链接/快速路径）。

## 6. 本文件的性质

**计划/清单文档**，未执行任何复制、未修改沙箱、未触碰现场。树保持冻结（`ninja -C build32 -n` = no work）；现场 DLL 仍 `A0A51AF2…`。
