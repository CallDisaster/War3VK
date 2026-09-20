# 2026-09-18 — M2-1 live palette selection 迁出：逐条等价对照、可执行等价测试与变异证据

> 范围：只补 **M2-1**（docs/plan/2026-09-18-relay-m2-t2t3-workorder.md §2；
> docs/plan/2026-09-16-device-semantic-responsibility-migration.md §3 M2 的第一片）的
> **等价性证据**：8 个符号（4 个 live palette 纯计算 helper + 4 个运行时配置 getter）
> 从 `src/d3d9/d3d9_device.cpp` 迁到新模块
> `src/d3d9/war3/semantic/war3_live_palette_selection.{h,cpp}`
> （命名空间 `dxvk::war3::semantic`，与 M1 模块相同，device.cpp 调用点经同一条
> using-directive 机械解析）。
> **不扩大迁移范围**：M2-2（选择链本体 `War3TryBuildLiveRuntimeGroupPalette`、
> `War3SemanticPaletteSource` enum、`War3LivePaletteBuild*` 类型、slot 缓存与两个
> 计数器）、M2-3（motion 诊断三函数）、M3、M4 **均未开始**，本文任何结论都不得
> 外推为"device.cpp 的语义职责已经迁完"。
> 本文只证明 **M2-1 的判定语义未变**；不证明画面、不证明性能、不证明实机、不声称
> 稳定版。未部署、未启动游戏、未触碰 E:\Work\Warcraft III、未 git 写。

---

## 0. 一句话结论

M2-1 迁出的 8 个符号：**逐条对照未发现任何判定语义差异**；唯一文本差异是一处
搬动痕迹（`War3SemanticPaletteDiagnosticsRuntime` 的 `inline` 去掉，M1 B 类先例，
行为不可观测）。宿主机差分测试在同一输入域上同时运行"迁移前实现"（pre-M2 工作树
快照逐字节原文）与"迁出后实现"，**2,380,985 个检查点 0 差异**；两条变异（哈希常量
改错 / env getter 默认值翻转）分别被差分测试与等价门禁**实测抓红**，还原后 SHA-256
证明源码逐字节回到原状（§5）。

---

## 1. 迁移前后身份与产物

| 项 | 值 |
| --- | --- |
| 迁移前 src/d3d9/d3d9_device.cpp | **未提交工作树文件**（M1 成果尚未提交，git 无对应 blob）；2,314,463 B / SHA-256 `2736335B42FCF4947B728F2907A04FE9F63EC2E5C8683A887D0785432E499F1B`；捕获时间 2026-09-17T21:11:53（机器本地时间，第 0 步实测）；逐字节副本留存于 `%TEMP%\device_pre_m2_1.cpp`（校验同 SHA） |
| 迁出后 d3d9_device.cpp | 2,310,648 B / SHA-256 `B70F3AF97CD3DB554DDB311E9B89F3C64B99996EF9ADEA4FBA4CA7D2CF6F98F1` |
| 新模块头 war3_live_palette_selection.h | 2,275 B / SHA-256 `EB8D8E91680B72303EDB5807B596D593D57283FCBF9A082EF6F6E3769279EA71` |
| 新模块实现 war3_live_palette_selection.cpp | 5,189 B / SHA-256 `C7B2434F3994CF89512C9B969A6319EFEFE5AAA377F63544E997610960A5D318` |
| legacy 参考（本文新增，§3.2） | 7,702 B / SHA-256 `D458C1AE6FF08B4962395CF77DC19D03AA6631FDDB05FC364002442A2A4F67D4` |
| 差分测试（本文新增） | 22,012 B / SHA-256 `3F3464BF8EA6D65FD74E626D2CCDE5489B2BE33F571C8EFF92A6447665E4D471` |
| 生成器（本文新增入库，§3.3） | 8,744 B / SHA-256 `3AFD20DAB4596AD21C5C7BCAE22B68EB09F692AE1AB69C51AC4B2797C9DED08B` |
| 等价证据门禁（本文新增） | 15,934 B / SHA-256 `7803FB1370245BB668250ED63B51ED520A684B94E9091D22D60CE8DBFD24108B` |
| 职责预算门禁（本轮修改，加法式） | 17,159 B / SHA-256 `3825A409E2C2B4C1CD49E9C264E43A1B0BDA578203CC206661EE06888FE54EBF` |
| 构建产物 build32/src/d3d9/d3d9.dll | 36,271,659 B / SHA-256 `D82AFF084C80BBE76F43DF6F6C1E9C0230EA59FCEFD473A329D6A7D2250A25F6`（变异还原后重建；首次构建 `E912665BB5…` 同长，仅链接时间戳差异，§7） |
| 差分测试可执行文件 | build32/src/d3d9/war3_live_palette_selection_test.exe（2,505,752 B，不入库） |

**provenance 与 M1 的差异（显式说明）**：M1 的迁移前文本是已提交的 git blob
（`ae890542d766:src/d3d9/d3d9_device.cpp`，blob SHA `3541DF5F…`）；M2-1 的迁移前
状态是 **M1 之后的未提交工作树**（M1 记录 §1 中"迁出后 d3d9_device.cpp"的同一
SHA `2736335B…`，可交叉验证 device.cpp 自 M1 起未变），git 里不存在该状态的
blob。因此本文与 .inc 头部记录的 provenance 是**工作树文件 SHA-256 + 捕获时间**，
不是 git blob 哈希；等价门禁以该工作树 SHA 与 .inc 自身 SHA 双钉死。

新增/修改文件：

- 新增 `src/d3d9/war3/semantic/war3_live_palette_selection.{h,cpp}`（迁移目标模块）；
- 新增 `src/d3d9/war3/semantic/tests/war3_live_palette_selection_legacy_reference.inc`
  （自动生成的迁移前实现参考，§3.2）；
- 新增 `src/d3d9/war3/semantic/tests/war3_live_palette_selection_test.cpp`（差分测试）；
- 新增 `AutoTest/gen_war3_live_palette_selection_legacy_reference.py`（生成器入库，
  补 M1 已知缺口）；
- 新增 `AutoTest/test_war3_live_palette_selection_equivalence_static.py`（防证据回退门禁）；
- 修改 `src/d3d9/d3d9_device.cpp`（删 8 个定义 + 1 个前置声明，加 include 与迁出注释）；
- 修改 `src/d3d9/meson.build`（`d3d9_src` 加模块 .cpp；新增测试 executable + test()）；
- 修改 `AutoTest/test_device_semantic_responsibility_budget_static.py`
  （**加法式**：`MODULE_HOME_PATHS` +2；新增 `FROZEN_M2_1` 表 + `FROZEN.update()`，§2.1）。
- 修改 `AutoTest/test_semantic_palette_diagnostics_hotpath_static.py`（**跟随符号**：
  该测试从 device.cpp 抽取 `War3SemanticPaletteDiagnosticsRuntime` 函数体断言
  env 默认值；符号迁出后改为从新模块 .cpp 抽取，断言内容一个字未改；
  device.cpp 侧的调用点断言全部保留）。

**M1 既有物一个字节未改**：`war3_device_semantic_predicates.{h,cpp}`、M1 legacy
参考 `.inc`、M1 差分测试、`AutoTest/test_device_semantic_predicates_equivalence_static.py`
全程未动（M1 等价门禁复跑全绿即证，§6）。

---

## 2. 迁移清单与逐条对照

### 2.1 清单（8 个符号，除此之外不迁）

| 符号 | 迁移前 device.cpp 位置 | 迁出后位置 | 文本判据 | 差异类别 |
| --- | ---: | --- | --- | --- |
| War3SemanticLivePaletteSafeCopyRuntime | :2071 定义 | cpp（运行时 getter 节） | 逐字节相同 | 无 |
| War3SemanticLivePaletteRefreshRuntime | :2081 定义 | cpp 同上 | 逐字节相同 | 无 |
| War3SemanticLivePaletteAllowCModelFallbackRuntime | :2088 定义 | cpp 同上 | 逐字节相同 | 无 |
| War3SemanticPaletteDiagnosticsRuntime | :2545 `inline` 定义 | cpp 同上 | 去 `inline` 后逐字节相同 | B 类（inline 去掉） |
| War3SemanticHashMatrixPalette | :1213 前置声明 + :7426 定义 | h 声明 + cpp 定义 | 定义逐字节相同（含签名续行 40 列缩进） | 前置声明由模块头声明承接（M1 同型） |
| War3DecodeRuntimePoseMatrix48 | :7445 定义 | cpp（纯计算节） | 逐字节相同 | 无 |
| War3TryReadRuntimePoseArray | :7454 定义 | cpp 同上 | 逐字节相同 | 无 |
| War3ResolveLivePoseRuntimeAlias | :7479 定义 | cpp 同上 | 逐字节相同 | 无 |

文本判据由脚本复核（迁移前快照 vs 新模块，签名锚抽取 + 逐字节比较）：
7 个定义 `IDENTICAL`，`War3SemanticPaletteDiagnosticsRuntime` 仅在去掉 `inline `
后 `IDENTICAL`。除此之外**没有任何一处改动**：无默认实参需要移动（8 个符号均
无默认实参），无函数内静态语义变化（4 个 env getter 的函数内 static 一次性读取
逐字节保留，函数从匿名命名空间普通定义变为模块 .cpp 普通定义，进程内仍然唯一
实例），无比较运算、常量、短路顺序、边界处理或副作用口径变化。

**B 类差异（唯一一处）**：`War3SemanticPaletteDiagnosticsRuntime` 迁移前是
device.cpp 匿名命名空间内的 `inline` 定义（匿名命名空间内 inline 本就冗余）；
迁出后是模块 .cpp 的普通定义（跨 TU 可见，不能只靠 inline）。可观测差异：无
（M1 记录 §3.3-B 同一类别，10 个先例）。每个函数只此一处定义（两处定义会链接
失败，实测链接通过）。

**device.cpp 侧（M1 先例）**：新增 `#include "war3/semantic/war3_live_palette_selection.h"`
（:142，M1 include 旁）；删除 :1213 前置声明（它与匿名命名空间绑定，保留会把
:7019 的调用绑到无定义的内部链接符号上）；4 处删除点各留一行迁出注释；
调用点文本**一个字符未改**，经 :151 既有 `using namespace dxvk::war3::semantic;`
解析（M1 同型，见 device.cpp :145-153 注释）。taxonomy 发射全部留在 device.cpp
调用侧，未随迁。

**预算门禁咬合**：`FROZEN_M2_1` 登记 8 个符号 budget=0（
`War3SemanticHashMatrixPalette` baseline=2：站点口径同时覆盖定义与前置声明，
迁移前 :1213 前置声明 + :7426 定义，与 `War3PacketIsPathBlocker (2, 0)` 同一先例；
工单文字"baseline 1"与门禁自身站点口径不一致，本文按门禁口径登记 baseline=2
并在此说明）；其余 7 个 baseline=1。判定 1（站点 ≤ budget）、判定 2（减少必须
落到模块）、判定 3（命名族回流）全部生效，实测"已迁出 34 个符号"（M1 26 +
M2-1 8）。`MIN_DEVICE_SITES=400` / `MIN_FROZEN_SYMBOLS=100` /
`MIN_MODULE_SYMBOLS=25` 均未改。

**`FROZEN_M2_1` 独立成表的原因（加法式、不削弱任何既有断言）**：M1 等价门禁
（本轮一个字节不改）按文本解析预算门禁的原始 `FROZEN` 块、以 budget<baseline
推导"已迁出集合"并对照其硬编码的 26 个 COVERED；M2-1 的 8 个符号若以
budget<baseline 写进该块，会被 M1 门禁误判为"已迁出但无 M1 等价证据"。因此
7 个命名族符号在原始块里保持 `(1, 1)` 登记不动，`FROZEN_M2_1` 以 budget=0
重复登记并经 `dict.update` 生效（预算门禁自身三条判定对 8 个符号照常咬合），
M1 门禁继续只钉 M1 的 26 个符号（复跑全绿，§6）。该结构已在预算门禁文件内
注释说明。

### 2.2 legacy 参考的可追溯性

`war3_live_palette_selection_legacy_reference.inc` 由入库生成器从 pre-M2 工作树
快照（`%TEMP%\device_pre_m2_1.cpp`，SHA 与 §1 登记值一致，生成器 fail-closed 校验）
**逐字节**抽取 8 个定义（按 device.cpp 原行号升序，与 2026-09-18 快照锚一致）：
只做三处机械处理——命名空间从 dxvk::anonymous 改为 `m2_legacy_reference`、
行尾 CRLF→LF 归一（device.cpp 为 CRLF；M1 的 git blob 同样是 LF 归一文本）、
以及 **supporting 定义** `War3GetEnvU32` 的抽取。

supporting 定义必须写清：`War3GetEnvU32` 属 M1（不在本轮 8 个符号内），M1 已把
它迁出 device.cpp，因此 pre-M2 的 device.cpp 里**不存在**它；它从 M1 模块
（`war3_device_semantic_predicates.cpp` :30）逐字节抽取——这正是 pre-M2 device.cpp
里 4 个 env getter 当时的真实调用目标。legacy 侧 env getter 与模块侧（经与 M1
模块逐字节相同的测试替身）解析到**文本相同**的实现，probe 场景再以独立期望值表
端到端校验该链（§4.3）。

### 2.3 生成器入库（补 M1 缺口）

M1 记录 §3.4 的生成器未入库是已知缺口；本轮把
`AutoTest/gen_war3_live_palette_selection_legacy_reference.py` 入库。它 fail-closed
校验 pre-M2 快照的 SHA-256 与字节数（不符即拒生成），按定义锚抽取（自动跳过
`War3SemanticHashMatrixPalette` 的前置声明），输出 .inc 并打印其 SHA-256 供门禁
登记。等价门禁额外断言 .inc 头部点名该生成器。

---

## 3. 可执行等价测试

### 3.1 形态

- 目标：build32/src/d3d9/war3_live_palette_selection_test.exe
  （meson 名 `warvk:war3_live_palette_selection`，83 个 meson 测试之一，超时 30 s，
  实测 < 3 s）。
- 源：tests/war3_live_palette_selection_test.cpp + **真实**模块
  war3_live_palette_selection.cpp + ../util/util_matrix.cpp（Matrix4 的
  `operator[]` 等 out-of-line 运算符；与 war3_runtime_group_palette_kernel_diff_test
  同一先例）。
- 测试文件为模块调用的设备层原语提供**宿主机替身**：可读内存范围表
  （`IsReadableRangeFast` / `IsReadableRange` 同一实现）、`dxvk::env::getEnvVar`。
  另：`sem::War3GetEnvU32` 由测试文件提供与 M1 模块**逐字节相同**的替身定义——
  War3GetEnvU32 属 M1 不在本轮范围，不链接 M1 模块 .cpp（那会拖入整套 M1 替身
  世界）；legacy 侧 .inc 内嵌从 M1 模块逐字节抽取的同一定义，两侧调用目标文本
  相同（§2.2）。**引用实现与模块实现共用同一份替身**，差分隔离出的正是 M2-1
  搬动的那部分逻辑。
- 测试文件与 legacy 参考都**不**包含 d3d9_device.cpp；不需要 Vulkan/D3D9 设备。

### 3.2 覆盖（同一输入 → 两个实现必须给出相同结果）

| 判定 | 输入域 | 检查点 |
| --- | --- | ---: |
| War3SemanticHashMatrixPalette | nullptr × count{0,1,1024,0xFFFFFFFF} + (ptr,0) + 特殊浮点位模式网格（+0/-0/±Inf/qNaN/sNaN/denormal 边界/1.0，size 1..16）+ 100,000 组随机 palette（10% 特殊位模式混入） | ~100.5k |
| War3DecodeRuntimePoseMatrix48 | 全 0/全 1/递增/0x80 递增 4 组边界 + 120,000 组随机 48 字节：每组 16 个分量位级对照 + 解码→palette 哈希往返对照 | ~2.04M |
| War3TryReadRuntimePoseArray | nullptr + 系统网格（模型可读长度 {0,0x5C,0x60,0x63,0x64,0x68,0x70} × count {0,1,2,8,255,256,1023,1024,1025,4096,0xFFFFFFFF} × pose {null,精确,未注册}）+ pose 可读长度边界（need-1/need-8/need+16）+ 40,000 随机场景；每场景对照返回值/outPoseCount/outPoseArrayPtr | ~120.9k |
| War3ResolveLivePoseRuntimeAlias | nullptr / 0x8000（<0x10000）/ 0x10000 未注册 + 三候选位单有效 × 5 种 count + 全无效/未注册 + 多有效顺序（+0xA0 优先、自身次之）+ pose 数组失效落低优先级 + 40,000 随机场景；每场景对照 resolved/count/arrayPtr | ~120.6k |
| 4 个 env getter | 主运行当前 env 下双侧对照 | 4 |

**实测结果（原始输出）**：

    war3 live palette selection equivalence: 2380985/2380985 checks passed
    RUN_EXIT=0

检查数量级对齐 M1（M1：3,098,194；本轮：2,380,985，下限断言 2,000,000）。

### 3.3 --probe：运行时配置 getter 的独立对照

函数内 static 使 env getter 一进程只读一次 env，因此单进程无法覆盖回退分支。
--probe 模式在同一进程内同时打印模块实现与 legacy 实现的值，门禁用 6 组 env
场景分别起进程验证"两侧一致"并对照**独立登记的期望值**：

| 场景 | 覆盖点 | 结果 |
| --- | --- | --- |
| default | SafeCopy=1、Refresh=1、AllowCModelFallback=0、PaletteDiagnostics=0 | module==legacy==期望 |
| flipped | 四值全翻：0/0/1/1 | 同上 |
| non-numeric | "abc"/"zzz" 无数字 → strtoul 无消费 → 回退默认（1/0） | 同上 |
| empty-string | 空字符串 → getEnvVar 为空 → 回退默认（Refresh=1、Allow=0） | 同上 |
| hex-base0 | "0x1" → 1（真）、"0x0" → 0（假）（strtoul base 0） | 同上 |
| nonzero-variants | "2" → 非零真、"07" → base 0 八进制 7 → 非零真 | 同上 |

此外门禁以 2 组 env 矩阵**整体重跑**差分测试（default 与 flipped-branches：
SAFE_COPY=0、REFRESH=0、ALLOW_CMODEL_FALLBACK=1、PALETTE_DIAGNOSTICS=1），
两组都必须 exit 0 且检查数 >= 2,000,000。

### 3.4 明确未覆盖（不得粉饰）

1. **替身不是生产实现**：本测试证明"同一输入 + 同一替身世界下两个实现一致"，
   **不证明**内存读取/env 原语本身正确；`sem::War3GetEnvU32` 是与 M1 模块
   逐字节相同的替身（不是链接 M1 模块真身），其语义由 probe 期望值表端到端
   校验、由 .inc 内嵌的逐字节同文定义对照。
2. **函数内静态的 env 分支**：单进程只能覆盖一组 env；已用 6 组 probe + 2 组
   差分矩阵覆盖主要分支，但**不是** env 全组合穷举。
3. **device.cpp 侧调用点编排与 taxonomy 发射不在覆盖范围**（taxonomy 对照属
   M2-5）；本测试不链接 device.cpp。
4. **合同 ON 分支（选择链本体消费这些 helper 的路径）未差分**：属 M2-2 范围
   （`War3TryBuildLiveRuntimeGroupPalette` 未迁）。
5. **不覆盖** GPU、Vulkan、实机画面与性能；不证明两份 `HashMatrixPalette`
   姊妹（war3_shadow_renderer_core.cpp / war3_model_hook.cpp）——S2 设计 §5-9
   仅登记不抽取，本轮未动。

### 3.5 无法在宿主机链接的部分与替代证据

- 模块 .cpp 本身可在宿主机编译并链接（实测），因此 **8 个符号没有一个是
  "只能靠文本对照"**：全部有可执行差分证据。
- 真正无法在宿主机运行的是**被替身替换的生产原语**（VirtualQuery/RPM 内存
  读取、env 读取）——它们由替身提供确定性输入，属于夹具边界，见 §3.4-1。

---

## 4.（预留：无）

---

## 5. 变异验证（真跑：改 → 构建 → 跑 → 红 → 还原 → 复跑绿）

两条变异都在 Below Normal + -j2 下构建（run_ninja_m2_1.cmd 单一构建入口），
构建 exit 0。

### 5.1 变异 (a)：把迁出的 FNV-1a 偏移基数改错

- 改动：war3_live_palette_selection.cpp 的 War3SemanticHashMatrixPalette 中
  `uint64_t hash = bit::fnv1a_init();` → `bit::fnv1a_init() + 1u`。
- 改动后模块 SHA-256：`45C9EBBCCDB7D9CA051F6C3E0C2A96DA4869D9FEF4ADAA44F8FA6F3BAF592E4B`
- 构建：NINJA_EXIT=0
- 差分测试（原始输出节选）：

      TEST_EXIT=1
      DIFF line 309 Decode48.roundtripHash: module=388902132087747322 legacy=7651642479114123787
      ...
      （DIFF 行合计 220041）
      war3 live palette selection equivalence: 2160944/2380985 checks passed

- 等价证据门禁（原始输出）：

      GATE_EXIT=1
      差分测试 env 矩阵 default 失败 exit=1

- **还原**：模块 SHA-256 回到
  `C7B2434F3994CF89512C9B969A6319EFEFE5AAA377F63544E997610960A5D318`
  （与改动前**逐字节相同**），NINJA_EXIT=0，§6 复跑全绿。

### 5.2 变异 (b)：把迁出的 env getter 默认值翻转

- 改动：war3_live_palette_selection.cpp 的
  War3SemanticLivePaletteAllowCModelFallbackRuntime 中
  `War3GetEnvU32("DXVK_WAR3_SEMANTIC_LIVE_PALETTE_ALLOW_CMODEL_FALLBACK", 0u)`
  默认值 `0u` → `1u`。
- 改动后模块 SHA-256：`CF75C9B5DD09CC84888737FB402402492D9DB28542F1CD0D62A50221BD9AF9FD`
- 构建：NINJA_EXIT=0
- 差分测试（default env 矩阵）：`2380984/2380985 checks passed` —— 恰好 1 个
  检查失败，即 DiffEnvGettersCurrentEnv 中该 getter 的双侧对照；等价门禁
  GATE_EXIT=1（`差分测试 env 矩阵 default 失败 exit=1`）。
- --probe（default env）实测：

      probe War3SemanticLivePaletteAllowCModelFallbackRuntime module=1 legacy=0

  probe 层的"module != legacy 即红"断言同样会杀死该变异（门禁在矩阵阶段
  已先行捕获，probe 未轮到执行）。
- **还原**：模块 SHA-256 回到
  `C7B2434F3994CF89512C9B969A6319EFEFE5AAA377F63544E997610960A5D318`
  （逐字节相同），NINJA_EXIT=0，§6 复跑全绿。

### 5.3 变异只证明"门禁/测试能抓这类回归"

两条变异分别覆盖：**纯计算常量改错**（差分测试与等价门禁矩阵）与
**env getter 默认值翻转**（差分测试的 env 感知对照与 probe 层）。它们**不**
证明其他类型的回归（替身原语错误、device.cpp 调用点编排错误、未识别命名
绕过）也会被抓。

---

## 6. 门禁原始输出（全部 Below Normal + -j2；变异还原后的最终复跑）

| 门禁 | 原始结果 |
| --- | --- |
| 构建 run_ninja_m2_1.cmd（src/d3d9/d3d9.dll + 差分测试目标） | NINJA_EXIT=0 |
| ninja -C build32 -n | ninja: no work to do. |
| 职责预算门禁 | EXIT 0：冻结符号 146；行首站点 迁移前 156 -> 迁出后上限 117 -> 当前 117；已迁出 34 个符号；device.cpp 行首定义站点总数 582 |
| M1 等价证据门禁（本轮一个字节未改） | EXIT 0：已迁出符号 26 个全部有 legacy 差分覆盖；legacy 参考 43452 B / SHA-256 2EA43F9782BE388E…；差分下限 3000000 断言；probe 场景 6 组 |
| **M2-1 等价证据门禁（新增）** | EXIT 0：已迁出符号 8 个全部有 legacy 差分覆盖；legacy 参考 7702 B / SHA-256 D458C1AE6FF08B49…；差分下限 2000000 断言；probe 场景 6 组 |
| 差分测试（默认 env） | war3 live palette selection equivalence: 2380985/2380985 checks passed，RUN_EXIT=0 |
| 记录器 war3_palette_object_evidence_test | SUMMARY: 23 passed, 0 failed，PROC_EXIT=0 |
| 成本 war3_palette_object_evidence_cost_test | COST_VERDICT=PASS checks=38 failures=0，SUMMARY: all checks passed，PROC_EXIT=0 |
| 生命周期 war3_shadow_build_lifecycle_test | SUMMARY: war3_shadow_build_lifecycle_test 187/187 case(s) passed，PROC_EXIT=0 |
| 解析器静态 AutoTest/test_palette_object_evidence_analysis_static.py | Ran 94 tests ... OK，PROC_EXIT=0 |
| 通用根读方静态 AutoTest/test_analyze_frame_evidence.py | Ran 55 tests ... OK，PROC_EXIT=0 |
| 往返 py AutoTest/test_palette_object_wire_roundtrip.py <exe> | ROUNDTRIP_VERDICT=CERTIFIED_SPEC_SATISFIED，SPEC_UNMET=[]，PROC_EXIT=0 |
| meson test -C build32 --num-processes 2 | Ok: 83  Fail: 0（M1 时点 82 + 本轮新增 war3_live_palette_selection），MESON_EXIT=0 |
| 全量静态 test_*_static.py | STATIC_TOTAL=249 STATIC_FAIL=0（M1 时点 248；新增 M2-1 等价证据门禁 1 个） |

---

## 7. DLL 身份

- 最终产物（变异还原后重建）：build32/src/d3d9/d3d9.dll，**36,271,659 B**，
  SHA-256 `D82AFF084C80BBE76F43DF6F6C1E9C0230EA59FCEFD473A329D6A7D2250A25F6`。
- 变异前同一源码的首次构建为 36,271,659 B / `E912665BB57A9D7DE8B47155756846304B83ADC97E2C7B2FACE0B96966F66DE2`；
  两次构建的源码逐字节相同（§5 还原以 SHA-256 证明），差异仅为 PE 链接时间戳
  ——本树早先已受控验证"源码逐字节相同的两次构建，DLL 差异仅为内嵌链接
  时间戳"（M1 记录 §7）。
- 对照迁移前候选：36,271,456 B / `CB62253417CEC91ADC9A16F7D5BAC8E758370C62DFBFEB63D84906FF031750AE`
  （工单 §0 实测）。尺寸 +203 B 与"新增一个模块 TU、device.cpp 净减 8 个定义"
  的构成变化一致。
- **未部署**：本轮没有覆盖任何游戏目录文件，未启动游戏、未启动编辑器。

---

## 8. 未做到 / 不确定 / 明确的措辞边界

1. **M2-2/M2-3/M3/M4 未开始**。M2-1 只迁出 8 个纯计算 helper 与 env getter。
   不能说"palette 选择链已迁完"，也不能说架构重构已完成。slot 缓存两个计数器
   的外部链接化（M1 D 类先例）属 M2-2，未做。
2. **taxonomy 对照属 M2-5**：本轮 taxonomy 发射全部留在 device.cpp 调用侧
   未动，未做迁移前后 taxonomy 计数对照（选择链本体未迁，无可对照入口）。
3. **合同 ON 分支差分覆盖属 M2-2**：选择链本体（含合同 ON 的消费路径）未迁，
   本轮差分只覆盖 8 个符号自身的输入域。
4. **差分是"同一输入域"证据，不是全输入证明**：输入域由枚举小域 + 固定种子
   随机组成（规模见 §3.2），未覆盖的输入组合仍然存在。
5. **替身世界是夹具**（§3.4-1）；`sem::War3GetEnvU32` 为逐字节相同替身而非
   M1 模块真身（§3.4-1）。
6. **env 分支非穷举**（§3.4-2）。
7. **未实机、未部署、未提交、未晋升稳定**。本文不含任何画面、性能或稳定性
   结论；新 DLL 身份仅按冻结纪律记录（§7）。
8. 变异验证只覆盖其登记的几条回归类型（§5），不证明其他类型回归也会被抓。
9. 数字口径：差分检查点数是**测试自报**的检查次数（含同一输入的多字段对照），
   不是"输入组合数"；--probe 是 6 组 env 场景，不是 env 全空间。
10. 本文的对照表基于**本次工作树状态**；若将来 M2-2/M2-3/M3/M4 改动
    device.cpp 或模块，必须重新生成 legacy 参考与差分覆盖，不能沿用本文结论。
11. 预算门禁文件仍混用 CRLF 与孤立 CR 行尾（M1 记录 §8-10 已登记）；本轮
    对其中插入的新文本使用 CRLF，门禁自身按 splitlines() 解析不受影响，
    M1 等价门禁的文本解析实测一致（§6 复跑绿）。
12. `FROZEN_M2_1` 与原始 FROZEN 块的 7 个同名条目是刻意的双登记（§2.1）；
    若将来 M1 等价门禁改写其解析口径，必须同步复核本结构。
