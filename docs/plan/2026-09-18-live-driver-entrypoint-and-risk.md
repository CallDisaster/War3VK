# 实机对照前置之三：可脚本化入口地图 + ⚠️ 双安装混淆风险 — 2026-09-18

> 承接 `...-deploy-restore-validated.md`。本文确定"如何从脚本驱动一次隔离桌面运行"，
> 并记录一个**必须先排除的安全风险**。

## 1. 入口已找到（绕过 MCP）

MCP 工具无法在纯脚本环境调用，但存在**直接函数调用**的既有先例：

```python
# AutoTest/run_frame_evidence_gate.py:118
launch = war3.launch_war3_test(war3_dir=str(GAME), map_path=str(out/'map.w3x'), windowed=True, ...)
```

即：`import war3_autotest_mcp as war3` 后直接调用 `war3.launch_war3_test(...)`。
同类先例还有 `AutoTest/run_gpu_skin_dual_isolated.py:472`。

另有 `AutoTest/run_frame_evidence_gate.py` 是**现成的取证门驱动**（自带"候选/现场/玩家身份精确核对 + 拒绝既有进程"的守卫），
但它面向 frame-ID canary，而非 palette 对象证据。

## 2. ⚠️ 双安装混淆风险（必须先排除）

`run_frame_evidence_gate.py` 顶部常量：

```python
GAME   = Path('E:/Work/War3')                      # <-- 另一个安装
PLAYER = Path('E:/Work/Warcraft III/d3d9.dll')      # <-- 我们的目标站点
GAME_SHA = 'E04D1716603C075EB0C8E1E21CF1093A664ADC5249EFAB396BFA08D7B09D0C3A'
```

| 安装 | 路径 | 本轮是否可动 |
| --- | --- | --- |
| **我们的目标站点** | `E:\Work\Warcraft III` | 是（用户已授权） |
| **另一个安装** | `E:\Work\War3` | **否** —— 该路径由既有驱动使用，其 `GAME_SHA` 与我们的现场不同，动它等于动了计划外的现场 |

⇒ **直接运行 `run_frame_evidence_gate.py` 是不安全的**：它会把候选部署到 `E:/Work/War3`。
⇒ 正确做法是**新写一个专用驱动**，显式把 `war3_dir` 指向 `E:\Work\Warcraft III`，
并且 `deploy_d3d9_before_launch=False`（部署由我们自己控制的、已验证可逆的那条链负责）。

## 3. 下一轮应写的驱动（规格）

```python
import sys; sys.path.insert(0, 'AutoTest')
import war3_autotest_mcp as war3

SITE  = r'E:\Work\Warcraft III'          # 只允许这一个安装
MAP   = r'...\(2)ConcealedHill.w3x'        # CJK 路径会崩；光影测试.w3x 会崩 NVIDIA 驱动

launch = war3.launch_war3_test(
    war3_dir=SITE, map_path=MAP, launcher_mode='direct',
    use_isolated_desktop=True, desktop_name='WarVK-P0', windowed=True,
    deploy_d3d9_before_launch=False,     # 部署由已验证可逆的链单独负责
    enforce_video_baseline=False, auto_perf_record=True,
    env_overrides_json={'DXVK_WAR3_FRAME_EVIDENCE':'1',
                        'DXVK_WAR3_FRAME_EVIDENCE_PALETTE_OBJECT':'1',
                        'DXVK_WAR3_FRAME_HISTORY_SELF_CONTAINED':'0'})
# 之后：控制面 arm|trigger|freeze|export（或热键 Ctrl+Shift+C）取导出
```

**必须加的一条断言**：启动前核对 `SITE` 下 DLL 的 SHA 属于允许集合
（基线 `F275545B…5CF07FF3` 或候选 `BBEF3BBC…DE0DE357A`），否则中止。
**必须加的一条 finally**：无论如何恢复现场 DLL 并核对 SHA。

## 4. 状态

| 项 | 值 |
| --- | --- |
| 入口（脚本化） | 已确认：`war3.launch_war3_test(...)` 直接调用 |
| 现成驱动可复用性 | **否** —— 它作用于 `E:/Work/War3`，混用会动错现场 |
| 部署/恢复链 | 已验证可逆 |
| 全量静态 / meson / runnable | 258-0 / 85-0 / 74-76 |
| 实机运行 | **未做** |
| 现场 DLL | 未改动（`F275545B…5CF07FF3`） |
| 未提交 / 未部署 | 是 |
| 目标 | **不可标记完成** |

## 5. 边界

本文只做**入口确认与风险排除**；**未**写驱动、**未**启动游戏、**未**部署。
把"动错安装"这一风险提前写下来的价值，高于仓促跑一次可能落在错误站点上的运行。