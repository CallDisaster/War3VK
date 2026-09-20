# 检查点 c1：批次 2–4 全绿树的身份与还原信息 — 2026-09-18

> 归档：`E:\Work\WarVK-checkpoint-c1-20260918.zip`（SHA-256 `AEBC7724F517988B3C98C02F6F1494F3B482A3BB2357E2B1379F67EB534510B2`）
> （初版归档 SHA 为 `25C10EEF…DD4E`；v3 读方修复只改 `AutoTest/*.py`，故 **src/根配置/DLL 身份不变**，仅归档中的 `AutoTest/` 载荷需要刷新，已重打包。）
> 内容：`src/`、`AutoTest/`（**排除 `artifacts/` 与 `_archive/`**）、`docs/`、`meson.build`、`meson_options.txt`、`build32/meson-info/`。
> 排除理由：`AutoTest/artifacts/` 是运行生成的大量 `.tga`/日志（首次打包因它触发"系统资源不足"而失败），不属于源码身份。

> 目标①要求：固定一个**完整可还原**的检查点，明确记录「测的是哪份源码 / 哪个配置 / 回退回到哪里」，
> 并**显式区分测试环境与玩家安装目录**。本文是 c0 之后、批次 2–4 与 B1 之后的**新基线身份**。
> **保存检查点不等于发布。**

## 1. 身份（可逐位核对）

| 对象 | 值 |
| --- | --- |
| **源码指纹**（966 个 `src/` 文件，含 `src/minhook/build/**`；按相对路径+内容 SHA-256 归并） | **`B363F1769E9EF2514E5DD7616FB27C0C93EE23A2318B843BF26B2F71ED010EE2`** |
| **根配置指纹**（`meson.build` + `meson_options.txt` + `.git/HEAD`） | `612B91B4C382701F68A43BBFC08F4024B21F705E7A7A50D2268BA9B2C4B0E5C3` |
| **候选 DLL** | `BBEF3BBC8F52088FD34D6B5F68FEFA111BAA31A7CB2331674AC3917DE0DE357A`（36,297,472 B） |
| git HEAD / 分支 | `ae89054` / `codex/v1.22-release-integration-20260914` |
| 工作树脏文件数 | 632（**不提交**，按用户约束「不用 git 写」） |

## 2. 构建配置（**实际生效值**，非默认值）

```
buildtype                             = release
warning_level                         = 2
b_ndebug                              = false
warvk_skin_palette_contract_candidate = True     <-- 目标④的关键：严格 palette 契约**已启用**
```

编译器（`build32/meson-info/intro-compilers.json`）：

```
host  : i686-w64-mingw32-gcc  (E:/Dev/MinGW/bin/)
build : gcc 15.2.0 (i686-posix-dwarf-rev0, MinGW-Builds)
```

⇒ 注意 `warvk_skin_palette_contract_candidate` 在 `meson_options.txt` 中**默认 false**，
但 build32 的实际值是 **True**。这一差别在本轮之前曾导致对"R1 修复是否可达"的误判
（严格契约开启 ⇒ 冷缓存 legacy 路径是死代码），因此**必须按实际值记录，不能按默认值推断**。

## 3. 测试环境 vs 玩家安装目录（显式区分）

| 角色 | 路径 | 本轮状态 |
| --- | --- | --- |
| **构建/测试环境** | `…\dxvk-v1.22-integration-20260914\build32` | 全量静态 258/0、meson 85/0、runnable 74/76 |
| **实机测试站点** | `E:\Work\Warcraft III` | 现场 DLL = 基线 `F275545B…5CF07FF3`（36,288,789 B） |
| **另一个安装（不得触碰）** | `E:\Work\War3` | 既有驱动 `run_frame_evidence_gate.py` 的默认目标；`GAME_SHA=E04D1716…` |

### 3.1 现场回退信息（"回退回到哪里"）

| 文件 | 大小 | 说明 |
| --- | --- | --- |
| `d3d9.dll`（现场） | 36,288,789 B | **基线**，SHA `F275545BAA65A0153804AC1CC0B3533B595DA24B5725488B8FFB9DDC5CF07FF3` |
| `d3d9.dll.519AFA69_backup_20260918` | 36,283,128 B | 更早版本备份 |
| `d3d9.dll.A0A51AF2_backup_20260918-092843` | 36,271,456 B | 更早版本备份 |
| `d3d9.dll.sha256.candidate` | 76 B | 既有校验文件 |

**回退目标**：若候选需要撤回，恢复到上述基线 SHA 即可。
已提供工具 `AutoTest/restore_site_dll.py`（**改名让路** + 直接覆盖双策略，仅在找到 SHA==基线的备份时动手）。
注意：现场现有两个备份都**不是**基线（SHA 不同），因此工具会（正确地）拒绝用它们自动恢复。

## 4. 本检查点已实测通过的套件（**精确**表述）

```
全量静态脚本 : 258 scripts, 0 failed
meson test   : 85 Ok / 0 Fail
Win32 runnable（独立运行）: 74 / 76（2 项为参数化探针，调用方式未记录）
ninja -C build32 -n : no work
```

**未运行**：全量 Win32 runnable 的"参数化探针正确调用"、TDR/ABBA、玩家前台门、**实机运行**。
⇒ 因此**不得**表述为"全门禁通过"。

## 5. 与 c0 的差异（为什么需要新检查点）

c0 之后本树发生了：批次 2（R2/R3）、批次 3（FirstSight 版本 3 协议）、批次 4（严格准入测试 + B1 基线重建 + R3 wire 形状回归修复）。
其中 **B1 改变了生成器与参考文件身份**（chain 参考 36,025 B / `02FF8AFE…` → **37,375 B / `3D258F48…`**），
**R3 修复改变了写方对块头与 counters 的输出**。因此 c0 的身份**已不再代表当前树**。

## 6. 本检查点仍未解决的事项

| 项 | 状态 |
| --- | --- |
| 批次 4 第④问「正常对象是否被误伤」 | 仍只有**读码 + 纯谓词**证据，**无运行期证据** |
| 实机非零 palette 导出 | **仍无**（批次 3 的 FirstSight 链也依赖它） |
| 冻结/发布 | **未冻结、未发布、未晋升稳定** |

## 7. 现场待清理的残留（不影响正确性）

```
E:\Work\Warcraft III\d3d9.dll.locked_candidate   36,297,472 B   被存活进程 War3 pid 42212 占用，退出后可删
War3.exe pid 42212                              仍存活（隔离桌面，本会话无权终止）
```
---

## 8. 身份更新（2026-09-18 场景 G 之后）

| 对象 | 旧 | **新** |
| --- | --- | --- |
| 源码指纹（932 文件） | `57D917F9…D4670ECD` | **`B363F1769E9EF2514E5DD7616FB27C0C93EE23A2318B843BF26B2F71ED010EE2`** |
| 根配置指纹 | `612B91B4…B0E5C3` | 不变 |
| 候选 DLL | `BBEF3BBC…DE0DE357A`（36,297,472 B） | **不变** |
| 归档 | `25C10EEF…` → `069B00F3…` | **`AEBC7724F517988B3C98C02F6F1494F3B482A3BB2357E2B1379F67EB534510B2`** |

**为什么源码指纹变了而 DLL 没变**：本轮改动只在**测试源码**（`war3_palette_object_wire_roundtrip_test.cpp`，新增场景 G）与 `AutoTest/` 驱动；测试 .cpp 不进 DLL ⇒ **现场部署用的候选二进制没有变**。
---

## 9. ⚠️ 指纹修正（2026-09-18，可还原性验证时发现）

本文早前记录的源码指纹（`57D917F9…` / `064B316A…`，**932 文件**）是用**有缺陷的过滤**算出的：
`-notmatch '\build'` 把 `src/minhook/build/**` 这 34 个**真实源码**（MinHook 的 Makefile/vcxproj 等）错误排除。

**正确值**：`B363F1769E9EF2514E5DD7616FB27C0C93EE23A2318B843BF26B2F71ED010EE2`（**966 文件**）。

归档本身**没有问题** —— 解包后的 `src/` 与仓库 `src/` 用同一方法比对，**指纹完全相同（MATCH = True）**。
详见 `2026-09-18-checkpoint-restorability-verified.md`。