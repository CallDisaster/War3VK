# M2-3 迁移等价记录：motion / churn 诊断三函数

> 状态：**完成（§1–§7 全部闭合）**。迁移逐字节落地；legacy 参考、宿主差分、
> 预算门禁登记（`FROZEN_M2_3`）、加法式等价门禁与独立 M2-3 门禁已闭合；
> 变异 3 条真跑（含 1 条可编译回流对照）；全量门禁在最终树复跑留档。
> 本轮不部署、不启动游戏、不提交 git、不称稳定版。原始门禁输出：
> `docs/plan/2026-09-18-m2-3-full-gate-rerun.log`。

工单：`docs/plan/2026-09-18-overnight-execution-plan.md` §2 M2-3、
`docs/plan/2026-09-18-deepseek-overnight-handoff.md` §4 阶段 3。同构模板：
`docs/plan/2026-09-18-m2-1-migration-equivalence-record.md`、
`docs/plan/2026-09-18-m2-2-migration-equivalence-record.md`。

## 1. 迁出范围（逐字节）

pre-M2-3 `src/d3d9/d3d9_device.cpp` 的 177 行相邻文本块
（:7344-7520，匿名命名空间内）逐字节迁入 M2-1 已建模块
`src/d3d9/war3/semantic/war3_live_palette_selection.{h,cpp}`
（命名空间 `dxvk::war3::semantic`）：

| 内容 | pre-M2-3 device.cpp 行号（含） | 行数 | 迁出后 device.cpp | 模块位置 |
| --- | --- | --- | --- | --- |
| `struct War3SemanticPaletteMotionEntry` | 7344-7349 | 6 | 0 行 | 模块 .cpp（实现细节，不导出） |
| `War3NoteLivePaletteMotion` | 7351-7414 | 64 | 0 行 | 模块 .cpp :812-875 |
| `struct War3SemanticHashMotionEntry` | 7416-7420 | 5 | 0 行 | 模块 .cpp（实现细节，不导出） |
| `War3NoteDrawTimePoseMotion` | 7422-7469 | 48 | 0 行 | 模块 .cpp :877-924 |
| `War3NoteSubmittedPaletteMotion` | 7471-7520 | 50 | 0 行 | 模块 .cpp :926-975 |
| （4 行分隔空行） | — | 4 | — | 随迁（保持逐字节相邻布局） |
| **合计** | 7344-7520 | **177** | **0** | **177 行** |

device.cpp 侧改为一条 6 行迁出注释（`// M2-3: … moved to …`）。五个调用点
（:19378 / :21066 / :21362 / :26877 / :26932）**一个字符未改**，经 M1/M2-1/M2-2
同一条 `using namespace dxvk::war3::semantic;`（device.cpp :156）机械解析。

模块侧新增内容：
- `.h`：`namespace dxvk { struct War3ShadowCaptureStats; }` 前向声明 +
  3 个函数声明（8,259 → 9,517 B）。**重量头 `d3d9_war3_scene.h` 不出现在 .h**
  （只前向声明），头文件 include 面不膨胀。
- `.cpp`：新增 `#include "../../d3d9_war3_scene.h"`（**只此一处**）+ 177 行
  逐字节文本（34,556 → 41,008 B）。

**唯一的机械差异**：三个函数的 `War3ShadowCaptureStats&` 参数由
`dxvk::` 匿名命名空间改为模块命名空间可见的同一类型（前向声明 → 定义在
device.cpp/模块 .cpp 两侧都通过 `d3d9_war3_scene.h` 取得完整定义）；函数体、
Entry 结构、判定顺序、读取口径**逐字节未变**（由迁移脚本对 .inc 做整块
`in` 比对证明，5/5 项命中且各只出现一次）。

## 2. provenance（fail-closed）与 baseline 实测

- pre-M2-3 工作树 `src/d3d9/d3d9_device.cpp` = **2,278,494 B**，
  SHA-256 `ECC4B828AF344DCF20368DAC9F6C8344CFCDE4B862779A0190C6390CB3F6F7B3`，
  捕获时间 `2026-09-18T02:18:25`（机器本地时钟；文档日期 2026-09-18）。
- 该 SHA 与 M2-2 记录 §2 的"迁移后 device.cpp"**完全一致**——pre-M2-3 状态
  就是 post-M2-2 工作树，交叉验证闭合。与 M1/M2-1/M2-2 相同：这是**工作树
  文件哈希**，不是 git blob（M1/M2-1/M2-2 的工作均未提交）。
- 快照备份：`%TEMP%\device_pre_m2_3.cpp` = 2,278,494 B / 同一 SHA。
- 迁移后 device.cpp = **2,273,048 B**，
  SHA-256 `D8211864549BBD830080C6C7A8BE4FCA222198DF7CAC92C5C096B8583AD1A7FE`。
- 迁移后模块 `.cpp` = **41,008 B**，
  SHA-256 `521BB85D32FEF05FC20114B40B8D484AD9790D58DE8626E3DF09DDF99B047359`；
  模块 `.h` = **9,517 B**，
  SHA-256 `EC3CFB5CB25EF85A3D8DC35DAE43D4E9126BD8EE1153C63CF0566D017A49EB82`。

**baseline 实测依据（预算门禁口径）**：用
`AutoTest/test_device_semantic_responsibility_budget_static.py` 内部**同一个**
`DEF_RE` 与同一"第 0 列起始、非注释行"规则扫描 pre-M2-3 device.cpp，得到

| 符号 | 行首站点 | 站点内容 |
| --- | --- | --- |
| `War3NoteLivePaletteMotion` | **1** | 定义首行 `:7351`（无前置声明） |
| `War3NoteDrawTimePoseMotion` | **1** | 定义首行 `:7422`（无前置声明） |
| `War3NoteSubmittedPaletteMotion` | **1** | 定义首行 `:7471`（无前置声明） |
| `War3SemanticPaletteMotionEntry` | 0 | 结构以 `{` 结尾，不匹配 `DEF_RE`（要求 `(`） |
| `War3SemanticHashMotionEntry` | 0 | 同上 |

⇒ `FROZEN_M2_3 = {三个符号: (1, 0)}`，**同一次改动**登记并迁移
（budget=0 只在迁移同时成立，不能先登记后迁移）。

**为什么必须显式登记**：这三个符号**不匹配**预算门禁的
`SEMANTIC_FAMILY_RE` 语义选择命名族（名字里没有
Resolve/Select/Choose/Decide/Classify/Score/Should/TryBuild/TryFind/
TryPopulate/Promote/CanPromote/IsEligible/Runtime/SelectionKey/Key/
ObjectKind/PathBlocker/LosBlocker 任何后缀）⇒ 判定 3（回流）对它们是**盲**的，
把任一函数重新内联回 device.cpp 不会被任何原有判定咬住。登记进
`FROZEN_M2_3` 后由**判定 1（预算）**点名。

## 3. legacy 参考

- 新文件 `src/d3d9/war3/semantic/tests/war3_live_palette_selection_motion_legacy_reference.inc`
  = **8,406 B**，
  SHA-256 `7DF61688219BAF83BCA1836FBEB97DC2D1269B103A703917718CA9C9177AC727`。
- 由入库生成器
  `AutoTest/gen_war3_live_palette_selection_legacy_reference.py --m2-3`
  从 `%TEMP%\device_pre_m2_3.cpp` 逐字节抽取（**fail-closed**：快照 SHA/字节数
  必须等于登记常量，否则拒绝生成）；含 5 项：2 个 Entry 结构（:7344/:7416）
  与 3 个函数定义（:7351/:7422/:7471）。M2-1 (`D458C1AE…`) 与 M2-2
  (`02FF8AFE…`) 的 .inc 与其 SHA **一字未动**。
- 头部 provenance 记录 pre-M2-3 工作树 SHA-256 / 字节数 / 捕获时间，并显式
  说明 worktree-vs-git-blob 差异（与 M2-1/M2-2 同口径）。
- 命名空间改写规则（.inc 头部逐条列明，**除 LF 归一外唯一机械编辑**）：外层
  命名空间改为 `m2_legacy_reference`；三函数内**无限定**的
  `War3SemanticPaletteDiagnosticsRuntime()` 解析到 M2-1 legacy 副本（即
  pre-M2-3 device.cpp 经 using-directive 的真实调用目标）；
  `War3ShadowCaptureStats` 是**共享真实契约类型**（两侧同一份
  `d3d9_war3_scene.h` 定义，刻意不重写）。**没有任何 token 被改写**。

## 4. 宿主差分等价测试

差分测试：`src/d3d9/war3/semantic/tests/war3_live_palette_selection_test.cpp`
（M2-1/M2-2 断言一字未动的**加法式扩展**）。meson 目标
`war3_live_palette_selection_test` 链接**真实模块 .cpp**（M2-1 既有目标，
未改 args），既有 `--probe`/`--probe-chain` 保留。

### 4.1 结构

- 三个函数**从不解引用** `runtimeModelPtr`（只做指针相等比较）⇒ 本电池
  使用合成指针常量，**不需要任何替身**；差分隔离出的正是 M2-3 搬动的文本。
- 两侧各持**独立的** `War3ShadowCaptureStats` 实例与**独立的**函数内
  static 512 项指向表 + 替换游标，由**同一调用序列**驱动 ⇒ 状态演化必须
  逐点一致。
- 比较口径：23 个 motion 字段（live palette motion 11 + draw-time pose 5 +
  submitted palette motion 7）**逐调用**对照；每段场景结束再对整个
  `War3ShadowCaptureStats` 做 `memcmp`（结构只含 POD / `std::array`，
  已用 `static_assert(std::is_trivially_copyable<…>::value)` 钉住；两侧都
  从 `{}` 起步），以捕获"写到了 motion 之外的字段"。

### 4.2 场景电池

- **系统网格**：`ptr ∈ {nullptr, 4 个合成指针}` × `hash/raw/group ∈
  {0, 1, UINT64_MAX, 0x123456789ABCDEF0}` × `frameSerial ∈ {0, 1, UINT64_MAX}`；
  live 走完整 3 维网格（早退三条件与"raw 变/不变 × group 变/不变"四态全覆盖），
  pose/submitted 走 2 维网格；再加同指针
  new→stable→raw-changed→group-changed 与 pose/submitted 三态逐级序列。
- **定种子随机世界**：先用 700 个互异指针顺序驱动 512 项指向表**填满并越过
  替换游标**（驱逐槽位选择必须一致），再做 **300,000 轮** splitmix64 定种子
  随机：指针池 1,700 个（同时覆盖"既有条目命中"与"驱逐"）、
  1/23 概率注入 nullptr、hash 池含 0（反复走早退分支）、frameSerial 随机。
- **合计 motion 电池 6,956,814 断言**（实测：default/contract-on/motion-on 与
  flipped-branches 的差值恒为 6,956,814 ⇒ 与 env 分支无关，覆盖同一路径集）。

### 4.3 实测结果

| env 矩阵 | 检查数（全过） | 备注 |
| --- | --- | --- |
| default | **17,314,411 / 17,314,411** | 诊断门 OFF：两侧均早退 |
| flipped-branches | **18,123,899 / 18,123,899** | 含 `PALETTE_DIAG=1`，函数体被走到 |
| contract-on | **13,826,299 / 13,826,299** | 合同 ON |
| motion-on | **17,314,411 / 17,314,411** | M2-3 专用矩阵（`PALETTE_DIAG=1`） |

### 4.4 --probe-motion 独立期望值表

15 个固定场景 × 2 组 env（`diagnostics-on` / `default`），逐场景要求
module==legacy **全部 23 字段**相等，并与门禁内**独立登记**的期望值表逐字段
核对（不是与 legacy 互相印证）。诊断门 OFF 时 15 个场景必须**全 0**
（证明 `if (!War3SemanticPaletteDiagnosticsRuntime()) return;` 是第一道门）。

## 5. 变异验证（阶段 6，已闭合，全部真跑）

三条变异都在**本树**上真跑，每条都完成"改 → 构建 → 跑 → 红 → 还原 →
复跑绿"全循环；还原以 SHA-256 逐字节证明（Below Normal + `-j2`，
原始输出见本节）。

### 5.1 变异 1：模块侧计数累加条件反转（差分必须红）

- **改**：模块 `.cpp` :860 的
  `if (entry->rawHash != rawHash)` → `if (entry->rawHash == rawHash)`
  （`War3NoteLivePaletteMotion` 的 raw-changed / raw-stable 计数条件反转）。
  变异后模块 `.cpp` SHA-256
  `D1AD4C9F767A9AF41BC78E3F4315A5FAA6CDC16C4280E5E1EB5C00E9033A862D`。
- **构建**：`run_ninja_m2_3.cmd src/d3d9/war3_live_palette_selection_test.exe`
  → `NINJA_EXIT=0`（**能编译**：这正是"语义漂移但不报错"的回归类型）。
- **跑（红）**：
  - 差分测试（`DXVK_WAR3_SEMANTIC_PALETTE_DIAGNOSTICS=1`）：
    `war3 live palette selection equivalence: 17519175/18123899 checks passed`
    （flipped-branches 矩阵），原始 DIFF 行：
    ```
DIFF line 2534 LiveGrid.liveRawChanged: module=1 legacy=0
DIFF line 2534 LiveGrid.liveRawStable: module=0 legacy=1
DIFF line 2534 LiveGrid.liveRawChanged: module=2 legacy=0
DIFF line 2534 LiveGrid.liveRawStable: module=0 legacy=2
    ```
  - 独立 M2-3 等价门禁（`--probe-motion` 独立期望值表）：exit 1，
    ```
--probe-motion 场景 diagnostics-on: live-stable
module=2,1,1,0,0,1,1560832,17,17,34,34,0,0,0,0,0,0,0,0,0,0,0,0
legacy=2,1,0,1,0,1,1560832,17,17,34,34,0,0,0,0,0,0,0,0,0,0,0,0（迁移前后不一致）
    ```
  - M2 等价门禁：exit 1，`差分测试 env 矩阵 flipped-branches 失败 exit=1` /
    `stdout: war3 live palette selection equivalence: 17519175/18123899 checks passed`。
- **还原**：从 `%TEMP%\module_post_m2_3.cpp` 复制回；还原后 SHA-256
  `521BB85D32FEF05FC20114B40B8D484AD9790D58DE8626E3DF09DDF99B047359`，
  与变异前**逐字节相同**（脚本比对 `MATCH=True`）。
- **复跑绿**：`17314411/17314411 checks passed`（`PALETTE_DIAG=1`，
  `EXE_EXIT=0`）；独立 M2-3 门禁 exit 0；M2 门禁 exit 0。

**还原过程暴露的一个真实陷阱（如实记录）**：第一次"复跑绿"**仍然红**
（`16709687/17314411`）。原因不是还原不完整，而是
`Copy-Item` **保留源文件的 LastWriteTime**（备份时间 02:19:27 早于变异目标的
`.obj` 时间 02:23:59），ninja 因此判定 `no work to do`、**没有重编译**，
测试二进制里仍是变异目标。把还原文件 `LastWriteTime` 置为当前时刻后重编译
即复绿。⇒ 本轮的"还原"步骤除 SHA-256 逐字节比对外，还**必须以一次真实重编译
+HASH 复核**确认二进制确实来自还原后的源码；只比源码 SHA 会得到假绿/假红。
（变异 2 的还原同样显式 touch 后再构建。）

### 5.2 变异 2：职责回流（预算门禁必须点名而红）

把 `War3NoteSubmittedPaletteMotion` 连同它依赖的
`struct War3SemanticHashMotionEntry` **verbatim**（取自 pre-M2-3 快照
:7416-7420 与 :7471-7520）重新插回 device.cpp 的 M2-3 迁出注释之后。

**2a：原样回流（匿名命名空间，与迁移前位置语义一致）**

- 变异后 device.cpp = 2,274,924 B，SHA-256
  `E003050496DB7EE9EF4093604930D52188A0FC6AEF284B538124C5F3196B5A53`。
- **构建 → 红（编译器也拒绝）**：`NINJA_EXIT=1`（9.4s，错误提前中止优化），
  ```
../src/d3d9/d3d9_device.cpp:21248:35: error: call of overloaded
'War3NoteSubmittedPaletteMotion(dxvk::War3ShadowCaptureStats&, void* const&,
uint64_t&, const uint64_t&)' is ambiguous
  ```
  （device.cpp 的匿名命名空间成员经隐式 using-directive 进入
  `dxvk`，与 `using namespace dxvk::war3::semantic;` 导入的模块声明在
  device.cpp 的调用点同层，故歧义。）
- **预算门禁 → 红并点名**：exit 1，
  ```
AssertionError: d3d9_device.cpp 中语义选择职责符号超过冻结预算（职责回流）：
  War3NoteSubmittedPaletteMotion: device.cpp=1 > budget=0
  ```
- 对照：**独立 M2-3 等价门禁仍 exit 0**（它不编译 device.cpp）——即"差分/
  等价证据"这一层看不见回流，**只有预算门禁**能点名它。

**2b：可编译回流对照实验（证明门禁是"编译器不拦"时的唯一捕手）**

- 同一份 verbatim 回流块放进具名命名空间
  `namespace warvk_m2_3_reflow_probe { … }`（定义行仍从第 0 列开始 =
  门禁的站点口径），从而不与 using-directive 导入的模块声明产生歧义。
- 变异后 device.cpp = 2,275,005 B，SHA-256
  `40C4F902B2DDB69E31C1BDC26A91BFDA94A21B918632A8A24902BB8038C70332`。
- **构建 → 绿**：`NINJA_EXIT=0`（49.6s，完整 device.cpp 编译 + 链接）。
- **预算门禁 → 红并点名**：exit 1，
  `War3NoteSubmittedPaletteMotion: device.cpp=1 > budget=0`。
- 对照：独立 M2-3 等价门禁 exit 0（同上）。
  ⇒ 在"能编译的回流"上，`FROZEN_M2_3` 的判定 1 是唯一咬住它的门禁；
  这也正是本轮必须补登记的原因（该符号不匹配语义选择命名族，判定 3 盲）。

### 5.3 还原完整性（SHA-256 证明）

| 被变异文件 | 变异后 SHA-256 | 还原后 SHA-256 | 逐字节相同 |
| --- | --- | --- | --- |
| 模块 `war3_live_palette_selection.cpp` | `D1AD4C9F…3A862D` | `521BB85D32FEF05FC20114B40B8D484AD9790D58DE8626E3DF09DDF99B047359` | **是** |
| device.cpp（2a） | `E0030504…6B5A53` | `D8211864549BBD830080C6C7A8BE4FCA222198DF7CAC92C5C096B8583AD1A7FE` | **是** |
| device.cpp（2b） | `40C4F902…C70332` | `D8211864549BBD830080C6C7A8BE4FCA222198DF7CAC92C5C096B8583AD1A7FE` | **是** |

## 6. 门禁与构建产物（阶段 5/7 全量复跑，已留档）

原始输出：`docs/plan/2026-09-18-m2-3-full-gate-rerun.log`
（复跑窗口 2026-09-18 02:30:43 → 02:31:29，机器本地时钟；全部在**最终树**上取数字：
两条变异均已还原、源码 SHA 与 §2/§5.3 一致）。

| # | 门禁 | 原始结果 |
| --- | --- | --- |
| 1 | `ninja -C build32 -n` | `ninja: no work to do.` `ninja -n exit=0` |
| 2 | 构建 `build32/src/d3d9/d3d9.dll`（Below Normal + `-j2`，`run_ninja_m2_3.cmd`） | `NINJA_EXIT=0` / `build exit=0`（变异还原后的真实构建实测 47.7s；日志内该步为 no-op 复核） |
| 3 | 预算门禁 | EXIT 0：`冻结符号 149；行首站点 迁移前 159 -> 迁出后上限 115 -> 当前 115；已迁出 38 个符号；device.cpp 行首定义站点总数 577`（M2-2 后为 146 / 156→115 / 35 / 580） |
| 4 | M1 等价门禁（本轮**一个字节不改**） | EXIT 0：`已迁出符号 26 个全部有 legacy 差分覆盖；legacy 参考 43452 B / SHA-256 2EA43F9782BE388E…` |
| 5 | M2 等价门禁（M2-3 加法式扩展后） | EXIT 0：`M2-1 8 个 + M2-2 链符号 1 个 + M2-3 motion 符号 3 个 + 2 个 Entry 结构全部有 legacy 差分覆盖；motion legacy 参考 8406 B / SHA-256 7DF61688219BAF83…；motion-on 矩阵下限 12000000` |
| 6 | **M2-3 等价门禁（本轮新增）** | EXIT 0：`M2-3 已迁出符号 3 个 + 2 个 Entry 结构全部有 legacy 差分覆盖；pre-M2-3 device.cpp 2278494 B / ECC4B828AF344DCF…；probe-motion 场景 2 组 × 15 场景；motion-on 差分 17314411 断言（下限 12000000）` |
| 7 | `meson test -C build32 --num-processes 2` | `Ok: 83  Fail: 0`，`meson exit=0`（**83→83**：M2-3 是既有目标的加法式扩展，未新增 meson 目标） |
| 8 | 全量静态 `AutoTest/test_*_static.py` 逐个 | `STATIC_TOTAL=250 STATIC_PASS=250 STATIC_FAIL=0`（**249→250**：新增 M2-3 等价证据门禁 1 个；其余为扩展既有门禁 / 跟随迁移改锚） |
| 9 | 记录器 `war3_palette_object_evidence_test` | `SUMMARY: 23 passed, 0 failed`，`PROC_EXIT=0` |
| 10 | 成本 `war3_palette_object_evidence_cost_test` | `COST_VERDICT=PASS checks=38 failures=0`，`SUMMARY: all checks passed`，`PROC_EXIT=0` |
| 11 | 生命周期 `war3_shadow_build_lifecycle_test` | `SUMMARY: war3_shadow_build_lifecycle_test 187/187 case(s) passed`，`PROC_EXIT=0` |
| 12 | 解析器静态 `AutoTest/test_palette_object_evidence_analysis_static.py` | `Ran 94 tests … OK`，`PROC_EXIT=0` |
| 13 | 通用根读方静态 `AutoTest/test_analyze_frame_evidence.py` | `Ran 55 tests … OK`，`PROC_EXIT=0` |
| 14 | 往返 `py AutoTest/test_palette_object_wire_roundtrip.py <exe>` | `CHECKS=1107 FAILURES=0`，`SPEC_SATISFIED=['A','B','C','D','E','F'] SPEC_UNMET=[]`，`ROUNDTRIP_VERDICT=CERTIFIED_SPEC_SATISFIED`，`PROC_EXIT=0` |
| 15 | 差分测试四矩阵（最终树复测） | default **17,314,411/17,314,411**；flipped-branches **18,123,899/18,123,899**；contract-on **13,826,299/13,826,299**；motion-on **17,314,411/17,314,411**（全部 EXIT=0） |

**跟随迁移改锚的既有门禁（断言语义未削弱）**：
`AutoTest/test_semantic_palette_diagnostics_hotpath_static.py` 原先从 device.cpp 取
三个 `War3Note*Motion` 的函数体做"诊断门先于空指针检查"断言；M2-3 后改为从模块
`.cpp` 取同一函数体（断言行一字未改，只改锚）。

**文档写入后的复跑（2026-09-18 02:3x）**：本记录 §1–§7 与 changelog / 进度日志写入
完成后再跑四个门禁一次，全部 **exit 0**（预算门禁 149 冻结 / 159→115 / 已迁出 38 / 站点 577；
M1 等价门禁；M2 等价门禁；M2-3 等价门禁 motion-on 17,314,411 断言）。即 §6 的数字来自
**含本文档最终内容**的树。

### 6.1 DLL 身份

- 最终产物 `build32/src/d3d9/d3d9.dll` = **36,271,207 B**，
  SHA-256 `0090B354659432BAC0D6F341A099987F9A15D1B2FC1602712F8D1D1A3E385193`。
- 对照 M2-2 后的仓库候选：36,270,708 B /
  `EB9D51DE025CE8173400CE9C860FA70AB81D1E106B594DD3BF853944C9426A11`
  ⇒ 尺寸 **+499 B**（device.cpp 净减 177 行定义、模块 .cpp 增 177 行 + 一个
  `d3d9_war3_scene.h` include 面；净增与模块额外 include 的构成变化一致）。
- 变异 2b（可编译回流）那次构建也曾产出 DLL，但源码已还原，不构成最终身份；
  最终身份来自**还原后**的真实构建（§5.3 SHA 证明）。
- **未部署**：本轮没有覆盖任何游戏目录文件；未启动游戏、未启动编辑器；
  现场 `E:\Work\Warcraft III\d3d9.dll` 未被触碰。

## 7. 未覆盖项（如实声明）

- **device.cpp 调用点编排不在覆盖范围**（M2-5）：5 个调用点的触发条件、
  参数装配顺序、以及 `War3ShadowCaptureStats` 字段的**消费端**
  （`war3_perf_monitor.cpp` / `war3_control_plane.cpp` /
  `war3_shadow_runtime_bridge.cpp`）本轮一个字节未改、也未被本电池覆盖。
- **Entry 结构布局**未被单独断言：两侧都是逐字节同一文本，但测试没有对
  `sizeof`/`offsetof` 做跨翻译单元比对（同一编译器、同一头文件序下由
  文本一致性保证）。
- 函数内 static 的 512 项表：随机电池覆盖填满与驱逐，但**不覆盖**
  `s_replaceCursor` 回绕到 2^32（需 2^32 次驱逐，结构上不可达于测试预算）。
- env 覆盖为**枚举矩阵**（4 组）而非穷举：`DXVK_WAR3_SEMANTIC_PALETTE_DIAGNOSTICS`
  的取值形态覆盖 0/1/未设置/十六进制/非数字/空串由 M2-1 的 `--probe` 表承担，
  本电池只覆盖 ON/OFF 两态。
- **本测试证明"同一输入 + 同一 stats 状态序列下两个实现一致"**，不证明
  env 原语本身、也不证明 device.cpp 的调用时机正确。
- **不覆盖 GPU、Vulkan、实机画面与性能**；未部署、未启动游戏、未做玩家
  前台验收。本轮全部证据为宿主机差分 + 静态门禁，不构成稳定版依据。
- M2-4/M2-5（余下调色板符号 + 调用点编排/taxonomy 计数对照）**未做**。本轮
  不得表述为"palette 侧职责已迁完"。
- **变异 2b 是"具名命名空间回流"对照实验**，它只证明预算门禁的**行首站点口径**
  能点名回流符号，不证明"回流后的 device.cpp 运行行为"——那段代码没有任何
  调用者。变异 2a（原样回流）连编译都过不了，因此"能编译且调用点真的改走
  device.cpp 副本"的形态**未被构造**，也未覆盖其运行时影响。
- **构建系统陷阱已实测**：`Copy-Item` 还原会保留旧 mtime，ninja 会判
  `no work to do`（§5.1）。本记录不声称"源码 SHA 相同 ⇒ 二进制来自该源码"；
  本轮每个还原点都做了**显式 touch + 真实重编译 + DLL/EXE 复核**。
- **观测（非本轮引入）**：差分测试里 `Check`/`CHECK` 原语在整个文件中从未被
  调用（只有定义与宏），本次全量重编译因此出现
  `warning: 'void {anonymous}::Check(bool, const char*, int)' defined but not used`。
  这是**迁移前既有的源码状态**（本轮只增不删，未移除任何 `CHECK(` 调用；grep
  全文件只有 `#define CHECK(expr)` 一处），M2-3 未修复它，也未据此改变任何断言。
- **差分是一次性进程内证据**：`--probe-motion` 的期望值表随场景指针公式
  （`0x100000 + index*0x100`，index 2000..2013）与本文件字段顺序耦合；三者
  任一改动都必须同步更新门禁期望值表，否则会以"期望不符"的形式红掉（这是
  有意的 fail-closed，但也是维护面）。
