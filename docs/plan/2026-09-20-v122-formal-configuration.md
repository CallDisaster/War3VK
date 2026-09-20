# v1.22 正式配置与离线配置门

日期：2026-09-20。状态：配置审计与 verifier 候选，未构建、未实机、未打包。

本文件属于 configuration-release-r1 白名单。它不修改共享 device/Meson/version/AGENTS/changelog，
也不触碰并行的 Render Stats 工作。

## 1. 一手基线

- 公开 v1.21 基线：`ae89054`（tag `v1.21.00`）。
- 当前工作树仍有 862 个 dirty 路径，全部保留，不作整树覆盖。
- `build-options/intro-buildoptions.json` 与
  `build32/meson-info/intro-buildoptions.json` 字节一致：38,987 B，SHA-256
  `59F0B3CE1ADE2ADFCF0148B77383C8160464889C2B13B79B217233D90809EBAA`。
- `build32/meson-private/cmd_line.txt` SHA-256
  `8FE147A31AF1F25DBF833D6CA9A554D50F8CF2B05C2C7E7E257778A7BBED2979`。
- 当前 `build32`/`build-options` 是内部诊断配置：
  `warvk_internal_frame_recorder=true`、
  `warvk_skin_palette_contract_candidate=true`、
  `warvk_data_collection_tree_dev=false`、`b_ndebug=false`、`strip=false`。
- v1.22 相对 v1.21 新增 3 个 Meson 选项：
  `warvk_data_collection_tree_dev`、`warvk_internal_frame_recorder`、
  `warvk_skin_palette_contract_candidate`。三者源码默认均为 `false`。
- 其余 v1.21 已有 `_dev`/candidate 选项默认均为 `false`。
- 版本源仍证明 `1.21.00`：`meson.build`、`RELEASE`、`version.h`、
  `src/d3d9/version.rc`、JAPI 显示文本、README/CHANGELOG。

## 2. 选项与产品矩阵

| 选项 | 源码默认 | player-release | internal-diagnostic |
| --- | ---: | ---: | ---: |
| `warvk_internal_frame_recorder` | false | false | true |
| `warvk_skin_palette_contract_candidate` | false | true | true |
| `warvk_data_collection_tree_dev` | false | false | false |
| `warvk_shadow_observers_dev` | false | false | false |
| `warvk_rts_shadow_candidate_dev` | false | false | false |
| `warvk_coherent_up_index_trim_dev` | false | false | false |
| `warvk_current_up_shadow_replay_dev` | false | false | false |
| `warvk_coherent_real_index_trim_dev` | false | false | false |
| `warvk_coherent_real_perf_candidate_dev` | false | false | false |
| `warvk_device_address_binding_report_dev` | false | false | false |
| `buildtype` | release | release | release |
| `b_ndebug` | false | if-release / true | false |
| `strip` | false | true | false |
| `native_glfw/sdl2/sdl3` | auto | disabled | auto |
| `enable_d3d9` | true | true | true |
| `enable_dxgi/d3d8/d3d10/d3d11` | true/false depending | false | false |

### 2.1 player-release（普通玩家）

必须显式设置：

```text
-Db_ndebug=if-release
-Dbuild_id=false
-Denable_dxgi=false -Denable_d3d8=false -Denable_d3d9=true
-Denable_d3d10=false -Denable_d3d11=false
-Dnative_glfw=disabled -Dnative_sdl2=disabled -Dnative_sdl3=disabled
-Dwarvk_internal_frame_recorder=false
-Dwarvk_skin_palette_contract_candidate=true
-Dwarvk_data_collection_tree_dev=false
-Dwarvk_shadow_observers_dev=false
-Dwarvk_rts_shadow_candidate_dev=false
-Dwarvk_coherent_up_index_trim_dev=false
-Dwarvk_current_up_shadow_replay_dev=false
-Dwarvk_coherent_real_index_trim_dev=false
-Dwarvk_coherent_real_perf_candidate_dev=false
-Dwarvk_device_address_binding_report_dev=false
```
`warvk_skin_palette_contract_candidate=true` 是保留的正确性路径，不可盲目设 false。
## 3. 根补丁提案与运行时门

### 3.1 b_ndebug

Meson 1.10 的 b_ndebug 默认 false；if-release 只在 release/plain 关闭断言。
当前诊断 introspection 为 false。产品必须显式 -Db_ndebug=if-release，验收
introspection 为 if-release 或 true，不能只看 buildtype=release。

### 3.2 native sync 与 perf recording 独立

`src/d3d9/war3/hooks/war3_native_capture.cpp` 的 PersistentNativeFrameSyncEnabled()
默认启用，只读 `DXVK_WAR3_NATIVE_FRAME_SYNC_PERSISTENT` 与
`DXVK_WAR3_NATIVE_FRAME_SYNC_ELIDE`；文件不依赖 `DXVK_WAR3_PERF_*` 或 isRecording。
关闭录制后普通空读回跳过仍应有效。

### 3.3 截图与内部测试 API

AsyncScreenshotEnabled() 默认 true；native 截图只在原生事件/显式测试命令到达时
有界复制和后台保存。DXVK_WAR3_INTERNAL_TEST_API 默认关，
kNativeInternalTestApiEnabled=false；普通玩家包不得把诊断截图变成默认常驻采集。

### 3.4 palette correctness

产品构建必须保留 `-Dwarvk_skin_palette_contract_candidate=true`。
ContractEnabled() 会默认选择严格 skin-palette publication 路径；不因去诊断而盲关。
源码 fallback 仍必须为 0，只有产品 recipe 显式打开。

### 3.5 排除项

Native auto-lights 仅 `DXVK_WAR3_NATIVE_MODEL_LIGHTS=1` 时 install，且仅
`DXVK_WAR3_NATIVE_MODEL_LIGHT_CONSUMER=1` 时消费；产品不设置这两个 env。
Water 分支不合并；x64 不产出，产品固定 build-win32.txt 和 --32-only。

### 3.6 package-release.sh 提案

在 build_arch() 的 meson setup 参数中显式加入：

```text
-Denable_dxgi=false -Denable_d3d8=false -Denable_d3d9=true
-Denable_d3d10=false -Denable_d3d11=false
-Dnative_glfw=disabled -Dnative_sdl2=disabled -Dnative_sdl3=disabled
-Dwarvk_internal_frame_recorder=false
-Dwarvk_skin_palette_contract_candidate=true
-Dwarvk_data_collection_tree_dev=false
-Dwarvk_shadow_observers_dev=false
-Dwarvk_rts_shadow_candidate_dev=false
-Dwarvk_coherent_up_index_trim_dev=false
-Dwarvk_current_up_shadow_replay_dev=false
-Dwarvk_coherent_real_index_trim_dev=false
-Dwarvk_coherent_real_perf_candidate_dev=false
-Dwarvk_device_address_binding_report_dev=false
```

保留已有 `-Db_ndebug=if-release`；调用方使用 `--32-only`。

## 4. Verifier 与负例

`AutoTest/release_v122_config.py` 只读检查 Meson options、已有 introspection、
生产 macro 守卫、skin/recorder/native-sync/screenshot/内部测试 API/native-light
源码默认、build-win32.txt、版本/ABI 和可选 package-release recipe。

`AutoTest/test_release_v122_config.py` 覆盖：recorder 泄漏、skin contract 盲关、
b_ndebug=false、dev 选项泄漏、intro 类型错误、Meson option 重复/缺省解析、
recipe 缺项或矛盾项、实际源码默认和版本/ABI 一致性。

离线命令（root 审批后）：

```bat
py -m py_compile AutoTest\release_v122_config.py AutoTest\test_release_v122_config.py
py AutoTest\test_release_v122_config.py -v
py AutoTest\release_v122_config.py --profile player-release --intro build32-product\meson-info\intro-buildoptions.json --expected-version 1.22.00 --recipe package-release.sh
```

## 5. 状态与未覆盖门

- 本 lane 只做正式选项/验证器准备，不 reconfigure build32，不产出产品 DLL。
- 无产品构建目录、无产品 introspection、无 clean DLL/no-work、无 Meson test。

* No native sync off-recording runtime, screenshot, or Froxel/local fog/Guide/JAPI combination test.
* No independent skin-palette dependency audit; only candidate=true must not be blindly disabled.

* Dirty count must be mode-labelled: earlier compact porcelain observed 862 paths;
  root -uall backup observed 912 paths. Do not use a single stale total.
* Original blocker: apply_patch wrapper via %* could not carry UTF-8/multiline
  reliably and long patches were truncated. Verified workaround: direct
  codex.exe --codex-run-as-apply-patch in small chunks. No shell source writes.

## 6. Machine-readable product profile and exact clean setup recipe

Optional partial-initialization image migration remains an unproven candidate;
this configuration gate does not state it is proven bad. Do not change its
status based on these diagnostic-default decisions.

Machine-readable player profile:

```json
{
  "profile": "player-release",
  "buildtype": "release",
  "b_ndebug": ["if-release", "true"],
  "strip": true,
  "debug": false,
  "optimization": "3",
  "build_id": false,
  "enable": {"enable_dxgi": false, "enable_d3d8": false, "enable_d3d9": true, "enable_d3d10": false, "enable_d3d11": false},
  "native": {"native_glfw": "disabled", "native_sdl2": "disabled", "native_sdl3": "disabled"},
  "warvk": {
    "warvk_internal_frame_recorder": false,
    "warvk_skin_palette_contract_candidate": true,
    "warvk_data_collection_tree_dev": false,
    "warvk_shadow_observers_dev": false,
    "warvk_rts_shadow_candidate_dev": false,
    "warvk_coherent_up_index_trim_dev": false,
    "warvk_current_up_shadow_replay_dev": false,
    "warvk_coherent_real_index_trim_dev": false,
    "warvk_coherent_real_perf_candidate_dev": false,
    "warvk_device_address_binding_report_dev": false
  }
}
```

Exact clean setup recipe (not executed, do not reconfigure current build32):

```bat
set "PATH=E:\Dev\MinGW\bin;E:\Dev\Vulkan SDK\Bin;D:\Environment\Python3.13.11\Scripts;%PATH%"
meson setup build32-product ^
  --cross-file build-win32.txt --buildtype release --strip ^
  --prefix out\WarVK-1.22.00 --bindir x32 --libdir x32 ^
  -Db_ndebug=if-release -Dbuild_id=false ^
  -Denable_dxgi=false -Denable_d3d8=false -Denable_d3d9=true ^
  -Denable_d3d10=false -Denable_d3d11=false ^
  -Dnative_glfw=disabled -Dnative_sdl2=disabled -Dnative_sdl3=disabled ^
  -Dwarvk_internal_frame_recorder=false ^
  -Dwarvk_skin_palette_contract_candidate=true ^
  -Dwarvk_data_collection_tree_dev=false ^
  -Dwarvk_shadow_observers_dev=false ^
  -Dwarvk_rts_shadow_candidate_dev=false ^
  -Dwarvk_coherent_up_index_trim_dev=false ^
  -Dwarvk_current_up_shadow_replay_dev=false ^
  -Dwarvk_coherent_real_index_trim_dev=false ^
  -Dwarvk_coherent_real_perf_candidate_dev=false ^
  -Dwarvk_device_address_binding_report_dev=false
```
