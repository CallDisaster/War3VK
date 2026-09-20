# 限制魔兽争霸3 的显卡占用（用户新增要求）— 2026-09-18

## 1. 需求

> 「追加一点，请你限制一下魔兽争霸3的显卡使用，前台其他应用被吃掉了很多性能导致无法运行。」

## 2. 现场诊断

```
Warcraft III            : 未运行（无占用）
GPU 占用第一名 pid 8812 : process = destiny2        <-- 用户自己的游戏，不是我的进程
                          59.7% engtype_3d
```

⇒ 你的前台应用就是 `destiny2`。此前我的取证运行（隔离桌面上的 War3，**不限帧**）
与它争抢同一块 GPU。**需要被限制的是我的测试运行**。

## 3. 采用的手段：DXVK 内联配置限帧（不落地文件）

`src/util/config/config.cpp:1664-1666`：

```cpp
// Load either $DXVK_CONFIG_FILE or $PWD/dxvk.conf
std::string filePath = env::getEnvVar("DXVK_CONFIG_FILE");
std::string confLine = env::getEnvVar("DXVK_CONFIG");     // 内联，按行解析
```

⇒ 用 **`DXVK_CONFIG` 环境变量**按次注入，**不写任何文件、不改动玩家安装**。
键取 `d3d9.maxFrameRate`（该键默认 `-1` 表示不限）。

我在取证驱动里加了：

```python
_max_fps = os.environ.get("WARVK_MAX_FPS", "30")     # 默认 30 FPS
if _max_fps not in ("", "-1"):
    env["DXVK_CONFIG"] = "d3d9.maxFrameRate = %s" % _max_fps
```

| 想做的 | 命令 |
| --- | --- |
| 默认（30 FPS） | `py AutoTest\live_contrast_palette_objects.py` |
| 更省显卡 | `$env:WARVK_MAX_FPS="20"` 后再跑 |
| 不限帧（仅在你不需要前台性能时） | `$env:WARVK_MAX_FPS="-1"` |

## 4. 为什么限帧是对的杠杆

取证关心的是**事件与链路**，不关心帧率；而 GPU 占用近似与帧率成正比。
限到 30 FPS 大致把渲染负载砍到原来的 1/2～1/3（取决于原帧率），
**取证数据不损失**（每个对象仍会产生 FirstSight/Enqueued/终态事件）。

## 5. 一次需要报告的失误

我为了检查驱动用法，运行了 `py AutoTest/live_contrast_palette_objects.py --help`；
但该脚本**没有参数解析**，于是它**直接开始执行运行流程**。
发现后我立即检查并终止：

```
War3 进程        : 无（未启动游戏）
驱动进程         : 无
站点 d3d9.dll    : 仍为基线 F275545B…5CF07FF3
dxvk.conf        : 不存在（无持久化改动）
```

未造成后果的原因：我用的 `| Select-Object -First 2` 提前关闭了管道，
驱动在**备份/部署之前**就被终止 ⇒ 没有部署、没有启动游戏。

⇒ 教训：**不要把"看帮助"当成无害操作**；对没有参数解析的脚本，
读取参数声明（或读文件）而不是执行它。

## 6. 现状

```
站点 d3d9.dll = 基线 F275545BAA65A0153804AC1CC0B3533B595DA24B5725488B8FFB9DDC5CF07FF3
无 War3 进程；无 dxvk.conf；无备份/停放残留
限帧只在**我下次启动取证运行时**生效（按次注入的环境变量）
```