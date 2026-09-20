# M2-2 迁移等价记录：调色板来源选择链本体

> 状态：**完成（7 个阶段全部闭合）**。迁移逐字节落地；既有门禁复绿；差分证据
> 闭合（3 组 env 矩阵全绿 + probe-chain 10 场景 × 3 env）；全量门禁复跑留档；
> 变异 2 条真跑（杀死 → 逐字节回退 → 复绿）。本轮不部署、不启动游戏、
> 不提交 git、不称稳定版。全量门禁原始输出：
> `docs/plan/2026-09-18-m2-2-full-gate-rerun.log`。

工单：`docs/plan/2026-09-18-relay-m2-t2t3-workorder.md` §2。同构模板：
`docs/plan/2026-09-18-m2-1-migration-equivalence-record.md`。

## 1. 迁出范围

把 pre-M2-2 `src/d3d9/d3d9_device.cpp` 中的调色板来源选择链本体**逐字节**迁入
M2-1 已建模块 `src/d3d9/war3/semantic/war3_live_palette_selection.{h,cpp}`
（命名空间 `dxvk::war3::semantic`）：

| 内容 | pre-M2-2 device.cpp 位置 | 迁入位置 |
| --- | --- | --- |
| `g_devicePaletteSlotCacheServedAfterConfirm/RejectedStaleCount` 两计数器定义 | :1060-1061 | 模块 .cpp（M1 D 类先例：唯一定义 + 头文件 extern；war3_diag Query 访问器留 device.cpp 经 using-directive 读取） |
| `War3SemanticPaletteSource`（7 值枚举）、`War3LivePaletteBuildPhase`、`kWar3LivePaletteBuildPhaseCount`、`War3LivePaletteBuildTiming`、`War3LivePaletteBuildRawTiming` | :5883-5967 | 模块 .h（逐字节） |
| `War3TryBuildLiveRuntimeGroupPalette` 前向声明（携带 `outSelection = nullptr` 默认实参） | :5969-5984 | 模块 .h（默认实参全部集中在头文件声明，定义不再重复——唯一的机械调整，与 M1/M2-1 同型） |
| `War3TryBuildLiveRuntimeGroupPalette` 定义（含 `resolvePaletteSlotIndex` lambda 的 4096 项 thread_local slot 缓存 + 8192 项 lookup 加速器 + Gap A producer 复核） | :7443-8064 | 模块 .cpp（函数体 25,663 字符，与 legacy 参考逐字节一致，脚本校验） |

device.cpp 侧删除上述定义并各留迁出注释；调用点经既有 using-directive 机械解析
（M1/M2-1 同型），调用点编排与 taxonomy 发射不在本轮范围（M2-5）。

**B 类搬动痕迹**：两计数器定义上方的旧注释尾行"声明位置提前到本匿名命名空间，供下方
war3_diag Query 访问器导出"描述的是 device.cpp 旧布局，迁出后不再成立，按 M1 B 类
先例删除；删除已在模块 .cpp 注释中说明。两条定义行本身逐字节未变。

## 2. provenance（fail-closed）

- pre-M2-2 工作树 `src/d3d9/d3d9_device.cpp` = **2,310,648 B**，
  SHA-256 `B70F3AF97CD3DB554DDB311E9B89F3C64B99996EF9ADEA4FBA4CA7D2CF6F98F1`，
  捕获时间 `2026-09-17T22:54:30`（机器本地时钟，与 M2-1 同口径；文档日期 2026-09-18）。
- 该 SHA 与 M2-1 记录的"迁出后 d3d9_device.cpp"**完全一致**——pre-M2-2 状态就是
  post-M2-1 工作树，交叉验证闭合。与 M1/M2-1 相同：这是**工作树文件哈希**，不是
  git blob（M1/M2-1 的工作均未提交）。
- 迁移后 device.cpp = **2,278,494 B**，SHA-256
  `ECC4B828AF344DCF20368DAC9F6C8344CFCDE4B862779A0190C6390CB3F6F7B3`。
- 迁移脚本一次性执行并做字节校验（删除区间、迁入区间的逐字节比对）；pre-M2-2
  快照备份 `%TEMP%\device_pre_m2_2.cpp`。

## 3. 依赖闭包清册（阶段 0）

链本体在 device.cpp 内依赖的外部符号共 **12 个**（差分测试为它们提供宿主机替身，
模块侧与 legacy 侧共用同一份替身）：

| 符号 | 声明位置 | 链内用途 |
| --- | --- | --- |
| `dxvk::war3::GetGameDllBase()` | `war3/war3.h:75` | Game.dll 基址（全局 palette 通路） |
| `dxvk::war3::SafeCopy(void*,const void*,size_t) noexcept` | `war3/core/war3_memory.h:27` | 全局 palette 快照复制（fail-closed） |
| `dxvk::war3::SafeReadU32Fast` / `SafeReadPtrFast` | `war3/core/war3_memory.h`（头内 inline，基于 `IsReadableRangeFast`，M2-1 已有替身） | part +0x08 slot 直读 / Game.dll 指针读 |
| `dxvk::war3::IsReadableRange` | 同上（替身已在 M2-1 存在） | 全局 palette 范围检查分支 |
| `model::QueryRenderablePartPaletteSlot` | `war3/model/war3_model_hook.h:499` | producer 绑定查询（Gap A 复核） |
| `model::QueryRenderablePartPaletteSnapshot` | 同 :507 | producer part palette snapshot（首选来源） |
| `model::QueryOwnedRenderablePartPaletteSnapshot` | 同 :512 | 合同 ON 唯一合法来源 |
| `model::QueryRenderablePartOwnerRuntimeModel` | 同 :521 | Phase 7.51 owner 反查 |
| `model::QueryBlendedPaletteBySlotIndex` | 同 :449 | blended slot palette |
| `model::QueryBlendedPaletteFrameTagRange` | 同 :490 | lazy frame tag 查询 |
| `model::PoseRegistry::instance()` / `findByRuntimeModel` | `war3/model/war3_model_registry.h:418/443` | 已发布 pose 记录 |
| `model::ShadowModelResourceCache::instance()` / `mapEpoch()` | `war3/model/war3_model_resource_cache.h:276/417` | slot 缓存的 map epoch 隔离 |

共享真实头（非替身）：`skin::Selection`（`war3/render/war3_skin_palette_selection.h`，
两侧共用同一合同类型）、`ShadowPacketResource`（只调三个 `*Vec()` 访问器）、
`dxvk::high_resolution_clock`（头内 inline 真 QPC；ticks 不比数值，只比 calls[]）、
`skin::ContractEnabled()`（头内 inline、函数内 static 读 env
`DXVK_WAR3_SKIN_PALETTE_CONTRACT`；测试目标未定义
`WARVK_SKIN_PALETTE_CONTRACT_DEFAULT` 宏 → 头内默认 0 → 默认 OFF；ON 覆盖由
contract-on env 矩阵分进程完成）。
`::GetModuleHandleA("Game.dll")` 是真实 WinAPI：测试进程从未加载 Game.dll，确定性
返回 NULL（"无 Game.dll"分支的宿主覆盖即来源于此，非替身）。

## 4. 符号对照（device.cpp → 模块）

| device.cpp（pre-M2-2） | 模块 | 口径 |
| --- | --- | --- |
| 匿名命名空间内两计数器定义 | `dxvk::war3::semantic` 命名空间内定义 + 头文件 extern | M1 D 类先例；war3_diag 访问器原位 |
| 匿名命名空间内类型块 | 头文件导出（逐字节） | 调用点经 using-directive 解析 |
| 匿名命名空间内链定义 | 命名空间内定义（函数体逐字节） | 默认实参集中到头文件声明 |

## 5. 差分证据

### 5.1 结构

- legacy 参考：`src/d3d9/war3/semantic/tests/war3_live_palette_selection_chain_legacy_reference.inc`
  = **36,025 B**，SHA-256
  `02FF8AFE5A30E3710D651BC300D1A7BB507B4DF34E6E6E394D6FA3CFD1D6410C`。
  由入库生成器 `AutoTest/gen_war3_live_palette_selection_legacy_reference.py --m2-2`
  从 pre-M2-2 快照逐字节抽取（fail-closed 校验快照 SHA/size），与 M2-1 .inc 共享
  `m2_legacy_reference` 命名空间。**生成器修正记录**：首版漏抽 :5969-5984 前向声明
  （携带第 17 参 `outSelection` 默认实参），导致"8-16 有默认实参而 17 没有"在单一
  声明下非法、宿主编译失败；已把前向声明**逐字节**补入（C++ 允许定义追加 8-16 的
  默认实参，与原文件形式一致），无任何为通过编译而做的文本编辑。M2-1 的 .inc 与其
  SHA `D458C1AE…` 未动。
- 差分测试：`src/d3d9/war3/semantic/tests/war3_live_palette_selection_test.cpp`
  （M2-1 断言一字未动的加法式扩展）。meson 目标链接真实模块 .cpp（M2-1 既有，
  未改）。stub 世界：part/model/pose 缓冲池 + 7 张可编程表 + Game.dll 伪镜像
  （12.36 MB，仅 +0xBC6BD0 处一枚指针）+ 全局 palette 缓冲（0x3A98×48 字节）+
  `g_stubMapEpoch`；12 个替身定义与生产签名精确一致（头内默认实参不重复）。
- 比较口径：返回值 / 全部输出参数（maxSlot/hash/rawPoseHash/poseModel/slotIndex/
  minTag/maxTag）/ palette **逐 float-word** / `timing.calls[]` 逐相位 / 两计数器
  **单调用增量** / `skin::Selection` 全 15 字段。**QPC ticks 不做数值比较**。
- 两侧函数内 static/thread_local（4096 缓存、8192 lookup、cursor、
  globalPaletteSnapshot、s_posePalette）各自独立，由同一调用序列驱动，状态演化
  一致；差分对缓存 confirm/reject 场景按调用逐次对照。

### 5.2 场景电池

- **55 个系统场景**：早退（both-null/empty-groups）、proven hint（<256/=0/
  不进入 GroupScan）、owned hit/miss（合同 ON 时 source=6 / fail-closed 不
  fallthrough；OFF 时 owned 表被忽略）、slot 直读 + part snapshot（hash=0 重算、
  frameTag=0 时 lazy FrameTagQuery ready/not-ready、空 palette 落 global）、
  global（null 指针/不可读/无 Game.dll 三条落 blended 变体）、blended（命中
  resize/欠数/超 256/尺寸不符四条拒绝变体）、part 不可读时 producer 绑定建条目/
  未绑定/绑定超 0x3A98、+0x08 超 0x3A98 的 outPaletteSlotIndex 发布、
  **缓存 confirm（ServedAfterConfirm+1）与三类 reject（槽位不一致/groupCount
  不足/producer miss，RejectedStale+1）**、epoch 翻转重绑、8192 lookup 碰撞
  （两 part 相距 131072 字节 → 线性扫描找回）、published pose（直中/+0xA0/
  -0xA0/owner 反查/owner 查询失败/owner==caller 守卫/hash=0 重算）、CModel
  alias（env deny/param allow/count>1024/pose 不可读/模型伪指针/poseCount
  不足走 uniform）、walk-through（published 与 cmodel 两种 source 的加权平均
  FP 路径 + groupSize=0/running 越界/decode 越界/slot≥groupCount/
  groupCount>256 五类无效变体）、5 条 fallback 各自隔离（directMatrixRemap/
  sparseMatrixRemap/directPose/sparsePose/uniform）。
- **20,000 轮固定种子随机世界**：8 个 part 指针复用（驱动缓存 confirm/reject/
  epoch 隔离）、slot08/绑定/snapshot/slot palette/frame tag/pose 记录/owner/
  owned/global/CModel 全表随机布点、proven∈{未知,精确,≥实际最大}（遵守生产
  上界合同）、epoch 5% 概率翻转。首轮随机电池曾把 proven 生成得**小于**实际
  最大槽位，触发链对 sparse 重建上界的越界写入（_GLIBCXX_ASSERTIONS 当场中止）；
  这是电池输入域违反而非链的新行为（链信任封包上界），已按合同修正生成器并
  在测试注释中记录。

### 5.3 实测结果（构建产物存在时由门禁复跑）

| env 矩阵 | 检查数（全过） |
| --- | --- |
| default | **10,357,597 / 10,357,597** |
| flipped-branches（SAFE_COPY=0/ALLOW_CMODEL=1/…） | **11,167,085 / 11,167,085** |
| contract-on（DXVK_WAR3_SKIN_PALETTE_CONTRACT=1） | **6,869,485 / 6,869,485** |

--probe-chain 10 固定场景 × 3 env：module/legacy 全 10 字段逐行相等，且与门禁
独立期望值表逐项一致（合同 ON：owned-hit → `1,6,42,…,77,77`；其余 9 场景全部
`0,0,4294967295,…` fail-closed；flipped-branches：cmodel-deny 翻转为
`1,5,4294967295,…`）。

### 5.4 未覆盖分支（如实声明）

- `requiredPaletteCount > 256` 早退：**不可达**（vertexGroups 为 uint8、proven
  <256 才生效，required ≤ 256 恒成立）——防御性死代码，电池无法覆盖。
- `!fallbackOk` 返回：alias/published 成功后 poseCount ≥ 1 恒成立，uniform
  总能成功——防御性死代码。
- 前向 alias 重试（:593-607 对 resolved ±0xA0 的 tryUsePublishedPose）：
  候选集合与 :561-572 完全相同，只会重复 miss（确定性）；电池覆盖其执行但
  其"重试命中"分支结构上不可达。
- 4096 cursor 回绕驱逐：随机电池仅复用 8 个 part 指针，不回绕；回绕只改变
  驱逐槽位选择，两侧由同一序列驱动逐点一致（构造保证）。
- 替身不是生产实现：本测试证明"同一输入 + 同一替身世界下两个实现一致"，不证明
  内存读取/env/producer 原语本身正确；env 覆盖为枚举矩阵而非穷举；
  `::GetModuleHandleA` 为真实 WinAPI（宿主确定性 NULL，见 §3）。
- device.cpp 调用点编排、taxonomy 发射、war3_diag 访问器行为不在本轮范围
  （M2-5）；不覆盖 GPU、Vulkan、实机画面与性能。

## 6. 变异验证（阶段 6，已闭合）

两条变异均完成"变异 → 杀死 → 逐字节回退（SHA 证明）→ 复绿"全循环
（Below Normal 构建，原始输出见留档日志）：

1. **对调 published-pose ±0xA0 候选次序**（模块 .cpp `:561-572`，先试 -0xA0
   再试 +0xA0）：差分 **10,228,430/10,357,581（129,151 项失败）**，DIFF 命中
   `poseModel`（两个候选都有记录时胜者翻转）、`rawPoseHash`、`paletteWord`；
   exe exit=1，等价门禁 exit=1。回退后 10,357,597/10,357,597 复绿。
2. **Gap A 跳过 producer 复核直接信任记忆槽位**（`useCachedEntry` 的三条件
   改为 `if (true)`）：差分失败，DIFF 精确命中设计——`slotIndex module=9
   legacy=4294967295`（变异侧供出记忆槽、legacy 拒绝）、`servedDelta 1 vs 0`、
   `rejectedDelta 0 vs 1`、相位 calls 差异；probe-chain `reject-then-published`
   场景翻转；exe exit=1，等价门禁 exit=1。回退后复绿。

回退完整性：每次回退后模块 .cpp SHA-256 还原为
`213A280C069145B6F89C254F5FDDC942229F515B7B4944628D8EAD121541FF3A`
（与变异前逐字节一致，脚本比对 MATCH）。

## 7. 门禁与构建产物（阶段 5 全量复跑，已留档）

原始输出：`docs/plan/2026-09-18-m2-2-full-gate-rerun.log`。

- DLL 构建 `NINJA_EXIT=0`；`build32/src/d3d9/d3d9.dll` = **36,270,708 B**。
  **DLL 哈希的构建非确定性说明**：链接器在 PE 头嵌入构建时刻
  TimeDateStamp（实测 = 构建当刻），同源字节重链会得到不同 SHA。阶段 5
  首次构建 SHA-256 `146B34A96ECABB28ADA89F812B2B5B9869AD59E52111A8BD0F6D962D3EC0BBB0`；
  阶段 6 变异回退（源码逐字节还原）后重链 SHA-256
  `EB9D51DE025CE8173400CE9C860FA70AB81D1E106B594DD3BF853944C9426A11`——
  尺寸相同、源码相同，差异来自嵌入时间戳，已实测 PE 头字段佐证。
- `ninja -C build32 -n` → `no work to do.` exit=0。
- 预算门禁 exit=0；M1 等价门禁（一字未改）exit=0；M2 等价门禁（M2-2
  加法式扩展后）exit=0。
- `meson test -C build32 --num-processes 2`：**83/83 Ok, 0 Fail**。
  （git-bash 直调 meson 会因缺 `ProgramFiles*` 在 `setup_vsenv` 崩溃——与
  `run_ninja_m2_2.cmd` 注释同源的已知环境问题；经 `env` 传递后通过，
  非测试失败，留档含两次失败与修正后的完整记录。）
- 全量静态门禁 `AutoTest/test_*_static.py` 逐个复跑：**PASS=249 FAIL=0**
  （无新增静态文件，M2-2 为扩展既有门禁）。
- 预算门禁：`FROZEN_M2_2 = {"War3TryBuildLiveRuntimeGroupPalette": (2, 0)}`
  （enum/类型/计数器不占 FROZEN 行——不匹配行首站点口径与命名族 regex，见
  预算门禁注释）；6 个断锚门禁按"跟随迁移改锚、断言语义不削弱"修复（改读模块
  .cpp/.h，断言一字未改）。
- 等价门禁（本记录的证据门禁）：`AutoTest/test_war3_live_palette_selection_equivalence_static.py`
  加法式扩展——链 legacy SHA/provenance、FROZEN_M2_2 ↔ CHAIN_COVERED、
  链符号三处出现、contract-on 矩阵、分矩阵检查数下限（default 7.2M /
  flipped 7.8M / contract-on 4.8M，≈实测 70%）、probe-chain 期望值表。
  M2-1 的断言、SHA、PROBE_SCENARIOS 一字未动。

## 8. 未做到项

- M2-3（motion 诊断迁移）、M2-5（调用点编排/taxonomy）未做。
- 未提交 git、未部署、未启动游戏、未做玩家前台验收；本轮全部证据为宿主机
  差分 + 静态门禁，不构成稳定版依据。
