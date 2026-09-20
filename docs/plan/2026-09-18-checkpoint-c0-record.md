# CHECKPOINT c0 — 2026-09-18（**未验收 / 非发布**）

> 本文件回答外部独立复审批次 1 的要求：**任何人都能明确回答「我们测的是哪份源码、哪个配置，回退会回到哪里」。**
> 保存检查点 ≠ 发布，也不冒充全部通过。

## 1. 身份

| 项 | 值 |
| --- | --- |
| 源码树 | `dxvk-v1.22-integration-20260914` |
| 分支 | `codex/v1.22-release-integration-20260914` |
| HEAD（**不含**本次任何改动） | `ae89054` |
| `src/` 文件数 | **965**（全部文件类型） |
| **`src/` 内容指纹 SHA-256** | `FC55FF8F070468B36A3EFAB2CDF8B45E31EB897AB3662F649502021EF11D9D1F` |
| **根构建配置指纹 SHA-256** | `6ED55D2BB3439CF16721DB2E72E4FF780483782D7684FECC0502A3EBDC9F41CD` |
| 产出 DLL | **36,288,789 B** / `FA7945A1F8C34DF37ED47820EFA7902CBD37200B0ECF26EAD0E26D6FE01162C3` |
| 存档 | `E:\Work\WarVK-checkpoint-c0-20260918.zip`（6.02 MB / SHA-256 `8DD64006A0E7735D7B27A61C34047C74CCD699AEB9D1C8B39C31AA426B6613C0`） |

**指纹算法**（可在任意机器独立复算）：

```
# src/ 内容指纹
对 src/ 下全部文件按相对路径排序，每行取「相对路径(正斜杠) 空格 SHA256(文件内容)」，
以换行拼接后取 UTF-8 字节的 SHA-256。

# 根构建配置指纹
同法处理：meson.build / meson_options.txt / .gitmodules / build32_safe.cmd /
build-win32.txt / build-win64.txt / AGENTS.md
```

本检查点**未提交**（遵守本项目「不用 git 写」约束），故以该指纹代替 commit id。

## 2. 实际生效的构建选项（`build32`，仅列 `warvk_*`，共 10 项）

完整副本见存档内 `_build-config/meson-info/`。关键项：

| 选项 | meson_options.txt 默认 | **build32 实际** |
| --- | --- | --- |
| `warvk_skin_palette_contract_candidate` | `false` | **`True`** |
| `warvk_internal_frame_recorder` | — | **`True`** |
| `warvk_shadow_observers_dev` | `false` | `False` |
| `warvk_rts_shadow_candidate_dev` | `false` | `False` |
| `warvk_data_collection_tree_dev` | `false` | `False` |
| 其余 5 项（coherent/up-index/device-address 等） | `false` | `False` |

### ⚠️ 正式答复外部复审的未决问题

复审指出：「严格检查的一部分由 `ContractEnabled()` 控制，头文件后备默认值是关闭，Meson 可以通过候选构建选项开启；本包没有根构建选项和实际运行配置，所以不能确认现场构建是否启用了它。」

**答复**：`warvk_skin_palette_contract_candidate` 在 `meson_options.txt` 中默认 `false`，但 **`build32` 的实际配置值为 `True`**。
因此**我们构建并实测部署的 DLL 确实启用了严格 palette 契约**。该选项值已随本检查点存档（`_build-config/meson-info/intro-buildoptions.json`）。

同时必须限定：这只说明**构建期启用了该契约**，不等于「每条实际运行路径都必然经过严格检查」——后者仍需按批次 4 用真实蒙皮选择入口做正常/异常对照验证。

## 3. 环境区分（显式，不再藏在 `sandbox` 名字后面）

| 用途 | 路径 | 说明 |
| --- | --- | --- |
| **玩家安装目录（真实）** | `E:\Work\Warcraft III` | 只做只读采集；任何写入必须先备份 |
| 构建 / 测试目录 | `...\dxvk-v1.22-integration-20260914\build32` | 构建产物与门禁执行地 |
| 隔离沙箱 | **无**（原 `E:\Work\War3_AutoTestSandbox` 已由用户删除） | AutoTest 沙箱根已改指向真实安装目录；**这是风险面，已记录** |

## 4. 回退点

| 备份 / 脚本 | 说明 |
| --- | --- |
| `E:\Work\Warcraft III\d3d9.dll.519AFA69_backup_20260918` | 上一候选，36,283,128 B |
| `E:\Work\Warcraft III\d3d9.dll.A0A51AF2_backup_20260918-092843` | 更早站点版，36,271,456 B |
| `E:\Work\Warcraft III\回退到A0A51AF2.cmd` | 回退脚本。**注意**：包内自带的「回退到旧版.cmd」指向 `E:\Work\War3` 副本，对现场无效 |

## 5. 已验证 / 未验证（不得混淆）

**本批次已跑（全部 PASS）**：

| 门禁 | 结果 |
| --- | --- |
| `war3_palette_object_wire_roundtrip_test` | `checks=116 failures=0 PASS` |
| `war3_palette_object_evidence_cost_test` | `COST_VERDICT=PASS checks=38 failures=0` |
| `war3_frame_evidence_runtime_test` | `runtime checks=723 PASS` |
| `war3_frame_recorder_memory_control_test` | `87 PASS` |
| `war3_shadow_build_lifecycle_test` | `187/187 case(s) passed` |
| `war3_shadow_geometry_domain_test` | `46/46 case(s) passed` |
| `AutoTest/test_independent_review_sep18_fixes_static.py` | **新增** R1/R2/R3 回归门禁 PASS |
| 既有 palette 静态门禁 ×3 | PASS |

### 验证方法（可复算）

1. 改动后先只构建 `d3d9.dll`；
2. **发现问题并纠正**：首次跑门禁时，测试 `.exe` 仍是**改动前**构建的（stale），该轮结果作废；
3. 执行 `ninja -C build32` **全量重建（22 个目标）**，确认 `ninja -C build32 -n` = **no work**；
4. **用重建后的二进制**重跑上表全部门禁 —— 下表即为该轮结果；
5. 重建后 `d3d9.dll` 哈希仍为 `FA7945A1…`，与本节记录一致 ⇒ **构建可复现**。

（第 2 步是自查发现：源文件已改而目标未重建时，门禁通过**不能**作为改动已验证的证据。此教训已写入开发日志。）

**未验证**：全量静态、meson 全量、Win32 runnable 全量、TDR/隔离 integrity、ABBA、玩家前台视觉与性能门。

⇒ **本检查点不是发布，不得表述为「全门禁通过」或「稳定版」。**

## 6. 与其它文档的关系

- `2026-09-18-openai-review-brief.md` —— 外部审核主报告；
- `2026-09-18-independent-review-response-20260918.md` —— 对外部复审三项缺陷的修复记录（R1/R2/R3）；
- `2026-09-18-mcp-driven-real-machine-feasibility.md` §31–§33 —— 记录器「拒绝优先」缺陷的实证与裁定请求。