# 2026-09-17 — M1 语义职责迁出：逐条等价对照、可执行等价测试与变异证据

> 范围：只补 **M1**（docs/plan/2026-09-16-device-semantic-responsibility-migration.md §3 第一步：
> 无 device 成员依赖的纯判定与常量）的**等价性证据**。
> **不扩大迁移范围**：M2（调色板选择统一入口）、M3（persistent geometry 准入/查找）、
> M4（CurrentDraw 绑定与 draw-time producer）**均未开始**，本文任何结论都不得外推为
> "device.cpp 的语义职责已经迁完"。
> 本文只证明 **M1 的判定语义未变**；不证明画面、不证明性能、不证明实机、不声称稳定版。
> 未部署、未启动游戏、未触碰 E:\Work\War3、未 git 写。

---

## 0. 一句话结论

M1 迁出的 26 个预算冻结符号 + 23 个随迁 helper：**逐条对照未发现任何判定语义差异**；
全部差异只有四类**搬动痕迹**（inline 增删、默认实参从定义移到声明、命名空间从
dxvk::anonymous 移到 dxvk::war3::semantic、全局计数器从内部链接改为外部链接），
其中没有一类改变返回值、求值顺序、短路行为、边界处理或副作用口径。
宿主差分测试在同一输入域上同时运行"迁移前实现"（git HEAD 逐字节原文）与"迁出后实现"，
**3,098,194 个检查点 0 差异**；两条变异（判定反转 / 职责回流 device.cpp）分别被
差分测试与预算门禁**实测抓红**，且还原后用 SHA-256 证明源码逐字节回到原状。

---

## 1. 迁移前后身份与产物

| 项 | 值 |
| --- | --- |
| 迁移前 src/d3d9/d3d9_device.cpp | git HEAD ae890542d766（工作树在此之上应用迁移，未提交）；52,115 行；blob SHA-256 3541DF5FE02EB9045B4A8499A1E7F2CBD2AB30DFF07896AFE1AAE43F1C847454 |
| 迁出后 d3d9_device.cpp | 2,314,463 B / SHA-256 2736335B42FCF4947B728F2907A04FE9F63EC2E5C8683A887D0785432E499F1B |
| 新模块头 war3_device_semantic_predicates.h | 12,214 B / SHA-256 514A6586A62101500712834917967D9886368E742B15AF81AECBEAC26AB12854 |
| 新模块实现 war3_device_semantic_predicates.cpp | 36,077 B / SHA-256 73C7D1A2185BE2FF77A80609CA6C39C1342ECFEC50F6D78ED0FD360AB1AACC0A |
| legacy 参考（本文新增，见 §3.4） | 43,452 B / SHA-256 2EA43F9782BE388E0FC3C98BDD8F60972354C0E8444EEF4A75E988C84E7B7968 |
| 差分测试（本文新增） | 65,379 B / SHA-256 5BEC10BF67BE47D8D5521E631E654647CC50D89CC01CC8B87144C1A8FC903AF6 |
| 等价证据门禁（本文新增） | 18,180 B / SHA-256 7C53E81E656BC1C86EC7DF79213673077F844502D33983013FFD5F8013CBB2AC |
| 职责预算门禁（上一轮产物，未改） | 15,441 B / SHA-256 DB9EDC23D09AB8A0E8C86E5D8EBA6A2738FE6BB1A8161AD67361F0850647C8AA |
| 构建产物 build32/src/d3d9/d3d9.dll | 36,271,456 B / SHA-256 CB62253417CEC91ADC9A16F7D5BAC8E758370C62DFBFEB63D84906FF031750AE |

新增/修改文件：

- 新增 src/d3d9/war3/semantic/tests/war3_device_semantic_predicates_legacy_reference.inc
  （自动生成的迁移前实现参考，见 §3.4）；
- 重写 src/d3d9/war3/semantic/tests/war3_device_semantic_predicates_test.cpp
  （原边界用例保留 + 差分等价 + --probe 模式）；
- 修改 src/d3d9/meson.build：该测试目标现在同时编译真实模块 .cpp
  （原目标只编译头文件内联部分；不链接真实实现就无法做差分）；
- 新增 AutoTest/test_device_semantic_predicates_equivalence_static.py（防证据回退门禁）。

**未修改任何判定语义**：war3_device_semantic_predicates.h/.cpp 在本文全部工作前后
SHA-256 保持不变（514A… / 73C7…）；d3d9_device.cpp 同样保持不变（2736…），
变异验证期间的临时改动已逐字节还原（§5）。

---

## 2. 迁移清单

预算门禁 AutoTest/test_device_semantic_responsibility_budget_static.py 把 M1 的迁出钉成
**26 个符号**（budget < baseline，实测"已迁出 26 个符号"）。这 26 个之外，还有 **23 个
随迁定义**（被它们调用、一起从 device.cpp 匿名命名空间搬进模块的判定/缓存 helper），
合计 49 个定义，全部在下表的对照范围内。

- **26 个预算冻结符号**：War3IsEligibleSemanticDynamicUnit、War3IsEligibleSemanticStaticWorldCaster、
  War3PacketIsPathBlocker、War3ResolveSemanticPacketObjectKind、War3ResolveSemanticPacketObjectKindFast、
  War3ScoreSemanticSceneFrame、War3SemanticDirectRecordSelectionKey、War3SemanticDirectSelectionKey、
  War3SemanticObjectGroupedSelectionRuntime、War3SemanticRejectAlphaBlendCasterRuntime、
  War3SemanticRejectUnsafeAlphaCasterRuntime、War3SemanticStickyPartSelectionMinRecordsRuntime、
  War3SemanticStickyPartSelectionRuntime、War3SemanticStickySelectionBroadLeasePreferenceRuntime、
  War3SemanticStickySelectionFillMarginRuntime、War3SemanticStickySelectionFillRuntime、
  War3SemanticStickySelectionLeaseFramesRuntime、War3SemanticStickySelectionLeaseRuntime、
  War3SemanticValidateUnitCoreRuntime、War3ShouldPreferSemanticSceneFrame、
  War3ShouldSubmitSemanticPacket、War3ShouldSubmitSemanticPacketFast、
  War3ShadowIsLosBlockerByJHandleFallback、War3ShadowIsLosBlockerByWidgetPtr、
  War3WidgetNegativeFrameCacheRuntime、War3WidgetProbeSafeCopyRuntime。
- **随迁定义**：War3GetEnvU32、War3SemanticByteSwapU32、War3SemanticFourCcHasPrefix、
  War3SemanticFourCcEqualEitherOrder、War3SemanticRawcodeLooksStaticWorldCaster、
  War3SemanticPacketHasStableUnitResource、War3IsSemanticUnitObject、War3SemanticReadUnitCore、
  War3SemanticUnitValidationCacheEntry、War3SemanticUnitValidationCacheSlot、
  War3SemanticPacketHasConsistentUnitCore、War3SemanticDirectPacketMatchesMainWorldVisibleRecord、
  War3SemanticDirectPacketHasMainWorldVisibleBacking、War3HasSemanticDynamicUnitEvidence、
  IsLosBlockerFourCc、War3WidgetNegativeFrameCacheEntry、War3WidgetNegativeFrameCacheSlot、
  War3WidgetNegativeFrameCacheHit、War3NoteWidgetNegativeFrameCache、
  g_pathBlockerEligibilityGateRejectCount、War3SemanticMaterialIsSafeOpaqueWorldCaster、
  War3LooksSubmitEligibleForScoringFast、War3WidgetNegativeFrameCacheTtlFrames。

---

## 3. 逐条等价对照

### 3.1 对照方法与判据

- **迁移前文本**：git show ae890542d766:src/d3d9/d3d9_device.cpp（迁移尚未应用）。
  当时这些定义位于 namespace dxvk { namespace { … } }（匿名命名空间，内部链接）。
- **迁出后文本**：war3_device_semantic_predicates.h（内联部分）与
  war3_device_semantic_predicates.cpp（其余部分），位于 dxvk::war3::semantic。
- **文本等价判据**（两条，脚本可复现）：
  1. ws：去注释、折叠空白后逐 token 相同；
  2. ns：在 ws 基础上再去掉命名空间限定符后逐 token 相同。
- 对 ws=False 的每一个符号都逐条人工核对差异**类别**，结果只有四类（§3.3）。

### 3.2 对照表（49 个定义）

文本列：ws=True = 去注释/空白后**逐 token 相同**；ws=False = 有差异，差异列给出类别；
所有 ws=False 的项在 §3.3 有逐类行为分析。

| 符号 | 迁移前 device.cpp 行 | 迁出后位置 | 文本 | 差异类别 |
| --- | ---: | --- | --- | --- |
| War3GetEnvU32 | 1132 | cpp:30 | ws=True | 无 |
| War3SemanticValidateUnitCoreRuntime | 1169 | cpp:41 | ws=True | 无 |
| War3SemanticObjectGroupedSelectionRuntime | 2341 | cpp:47 | ws=True | 无 |
| War3SemanticStickySelectionLeaseRuntime | 2473 | cpp:53 | ws=True | 无 |
| War3SemanticStickySelectionLeaseFramesRuntime | 2479 | cpp:59 | ws=True | 无 |
| War3SemanticStickySelectionBroadLeasePreferenceRuntime | 2486 | cpp:66 | ws=True | 无 |
| War3SemanticStickySelectionFillRuntime | 2494 | cpp:74 | ws=True | 无 |
| War3SemanticStickySelectionFillMarginRuntime | 2500 | cpp:80 | ws=True | 无 |
| War3SemanticStickyPartSelectionRuntime | 2507 | cpp:87 | ws=True | 无 |
| War3SemanticStickyPartSelectionMinRecordsRuntime | 2654 | cpp:93 | ws=True | 无 |
| War3SemanticRejectUnsafeAlphaCasterRuntime | 2938 | cpp:101 | ws=True | 无 |
| War3SemanticRejectAlphaBlendCasterRuntime | 3122 | cpp:116 | ws=True | 无 |
| War3WidgetProbeSafeCopyRuntime | 3411 | cpp:128 | ws=False | inline 去掉（B 类） |
| War3WidgetNegativeFrameCacheRuntime | 3423 | cpp:140 | ws=False | inline 去掉（B 类） |
| War3WidgetNegativeFrameCacheTtlFrames | 3433 | cpp:150 | ws=False | inline 去掉（B 类） |
| War3SemanticDirectSelectionKey | 2673 | h:38 | ws=False | inline 加上（A 类） |
| War3SemanticDirectRecordSelectionKey | 2753 | cpp:165 | ws=False | 默认实参移到声明（C 类） |
| War3IsSemanticUnitObject | 4680 | h:127 | ws=False | inline 加上（A 类） |
| War3SemanticByteSwapU32 | 4741 | h:132 | ws=True | 无（constexpr，未加 inline） |
| War3SemanticFourCcHasPrefix | 4746 | h:137 | ws=False | inline 加上（A 类） |
| War3SemanticFourCcEqualEitherOrder | 4754 | h:145 | ws=False | inline 加上（A 类） |
| War3SemanticRawcodeLooksStaticWorldCaster | 4760 | h:151 | ws=False | inline 加上（A 类） |
| War3SemanticReadUnitCore | 4771 | cpp:267 | ws=True | 无 |
| War3SemanticPacketHasStableUnitResource | 4800 | h:162 | ws=False | inline 加上（A 类） |
| War3SemanticUnitValidationCacheEntry | 4809 | cpp:245 | 逐字节 | 无（结构体；宿主脚本按函数名抽取故未纳入自动判据，已人工逐行比对） |
| War3SemanticUnitValidationCacheSlot | 4818 | cpp:254 | ws=True | 无 |
| War3SemanticPacketHasConsistentUnitCore | 4829 | cpp:296 | ws=True | 无 |
| War3SemanticDirectPacketMatchesMainWorldVisibleRecord | 4915 | cpp:336 | ws=False | 默认实参移到声明（C 类） |
| War3SemanticDirectPacketHasMainWorldVisibleBacking | 4975 | cpp:396 | ws=False | 默认实参移到声明（C 类） |
| War3HasSemanticDynamicUnitEvidence | 5036 | cpp:430 | ws=True | 无 |
| War3IsEligibleSemanticDynamicUnit | 5027 | cpp:421 | ws=True | 无 |
| IsLosBlockerFourCc | 9546 | cpp:758 | ws=False | inline 去掉（B 类） |
| War3ShadowIsLosBlockerByJHandleFallback | 9567 | cpp:762 | ws=False | inline 去掉（B 类） |
| War3WidgetNegativeFrameCacheEntry | 9606 | cpp:779 | 逐字节 | 无（结构体，同上人工比对） |
| War3WidgetNegativeFrameCacheSlot | 9611 | cpp:784 | ws=False | inline 去掉（B 类） |
| War3WidgetNegativeFrameCacheHit | 9622 | cpp:795 | ws=False | inline 去掉（B 类） |
| War3NoteWidgetNegativeFrameCache | 9635 | cpp:808 | ws=False | inline 去掉（B 类） |
| War3ShadowIsLosBlockerByWidgetPtr | 9646 | cpp:821 | ws=False | inline 去掉 + 默认实参移到声明（B+C 类） |
| War3PacketIsPathBlocker | 10602 | cpp:886 | ws=False | inline 去掉（B 类） |
| g_pathBlockerEligibilityGateRejectCount | 5025 | h:256 / cpp:908 | 见 §3.3-D | 内部链接改外部链接（D 类） |
| War3SemanticMaterialIsSafeOpaqueWorldCaster | 7857 | cpp:526 | ws=True | 无 |
| War3IsEligibleSemanticStaticWorldCaster | 7880 | cpp:549 | ws=True | 无 |
| War3ResolveSemanticPacketObjectKind | 7925 | cpp:594 | ws=True | 无 |
| War3ResolveSemanticPacketObjectKindFast | 7730 | cpp:469 | ws=True | 无 |
| War3ShouldSubmitSemanticPacket | 7972 | cpp:641 | ws=True | 无 |
| War3ShouldSubmitSemanticPacketFast | 7743 | cpp:482 | ws=True | 无 |
| War3LooksSubmitEligibleForScoringFast | 7759 | cpp:498 | ws=True | 无 |
| War3ScoreSemanticSceneFrame | 8038 | cpp:701 | ws=True | 无 |
| War3ShouldPreferSemanticSceneFrame | 8057 | cpp:720 | ws=True | 无 |

### 3.3 全部差异类别与行为分析（四类，逐一）

**A 类：inline 被加上**（6 个：War3SemanticDirectSelectionKey、War3IsSemanticUnitObject、
War3SemanticFourCcHasPrefix、War3SemanticFourCcEqualEitherOrder、
War3SemanticRawcodeLooksStaticWorldCaster、War3SemanticPacketHasStableUnitResource）。

- 迁移前：device.cpp 内的普通（非 inline）函数定义。
- 迁出后：模块头文件里的 inline 定义。
- 可观测差异：**无**。这 6 个函数都无函数内静态、无副作用；inline 只影响链接/ODR。
  调用点（device.cpp 及其它 TU）看到的函数签名与语义逐 token 相同。

**B 类：inline 被去掉**（10 个：War3WidgetProbeSafeCopyRuntime、
War3WidgetNegativeFrameCacheRuntime、War3WidgetNegativeFrameCacheTtlFrames、
IsLosBlockerFourCc、War3ShadowIsLosBlockerByJHandleFallback、
War3WidgetNegativeFrameCacheSlot、War3WidgetNegativeFrameCacheHit、
War3NoteWidgetNegativeFrameCache、War3ShadowIsLosBlockerByWidgetPtr、War3PacketIsPathBlocker）。

- 迁移前：device.cpp 中的 inline 定义（当时在匿名命名空间内，inline 是冗余的）。
- 迁出后：模块 .cpp 中的普通定义（跨 TU 可见，故不能只靠 inline）。
- 可观测差异：**无**。其中三个函数含函数内 static（env getter 的一次性读取、
  War3WidgetNegativeFrameCacheTtlFrames 的 TTL）：
  - 迁移前：inline 函数内的静态 → 程序内唯一实例（匿名命名空间只在本 TU，仍然唯一）；
  - 迁出后：普通函数内的静态 → 同样唯一实例。
  两者都是"首次调用读一次 env 之后固定"，进程内可观测行为一致（差分测试与 --probe
  在 6 组 env 场景下逐项验证）。
- 每个函数只此一处定义（若两处定义会直接链接失败，实测链接通过）。

**C 类：默认实参从函数定义移到头文件声明**（4 个：War3SemanticDirectRecordSelectionKey、
War3SemanticDirectPacketMatchesMainWorldVisibleRecord、
War3SemanticDirectPacketHasMainWorldVisibleBacking、War3ShadowIsLosBlockerByWidgetPtr）。

- 迁移前：定义自身携带默认实参（= nullptr / = false / = 0u）。
- 迁出后：模块头文件的**声明**携带完全相同的默认实参；.cpp 定义不再重复（C++ 规定
  同一作用域内默认实参只能出现一次）。
- 可观测差异：**无**。所有调用点看到的是头文件声明，默认值与参数位置逐 token 相同。
  差分测试对这 4 个函数同时覆盖"显式传参"与"省略尾参"两种调用形态。

**D 类：全局计数器的链接属性变化**（g_pathBlockerEligibilityGateRejectCount）。

- 迁移前：namespace dxvk { namespace { inline std::atomic<uint32_t>
  g_pathBlockerEligibilityGateRejectCount{0u}; } } —— 匿名命名空间内、内部链接，
  进程内唯一实例，零初始化。
- 迁出后：模块头文件 extern std::atomic<uint32_t> g_pathBlockerEligibilityGateRejectCount;
  加模块 .cpp 中的唯一定义 std::atomic<uint32_t> g_pathBlockerEligibilityGateRejectCount{0u};
  —— 外部链接，进程内**仍然唯一实例**，零初始化。
- 语义：只从内部链接变成外部链接；计数语义（fetch_add(1, relaxed) 累加、device.cpp
  在 shadow scene reset 时 exchange(0, relaxed) 取走累加值）逐 token 未变。
- 调用点核实：d3d9_device.cpp:21101/21103 仍以同一名字读取/取走该计数器，经
  using namespace dxvk::war3::semantic;（device.cpp:151）解析到模块内的同一实例；
  模块 .cpp:659 在 eligibility 拒绝路径上 fetch_add。排除了"两个计数器"的风险。

**其余差异**：无。**没有任何一条改动触及比较运算、常量、短路顺序、边界判断或副作用口径**。

### 3.4 legacy 参考的可追溯性

war3_device_semantic_predicates_legacy_reference.inc 由脚本从
git show ae890542d766:src/d3d9/d3d9_device.cpp **逐字节**抽取上述 49 个定义（按 device.cpp
原行号），只做两处机械处理：

1. 外层命名空间从 dxvk::anonymous 改为 m1_legacy_reference（文本本身未改）；
2. 三个 M1 类型（War3SemanticDirectSelectionKeySource、War3SemanticDirectMainWorldBackingStatus、
   War3SemanticSceneFrameScore）在该命名空间内**等值重声明**。

第 2 点的原因必须写清：若引用模块的类型，实参依赖查找（ADL）会把这些类型的命名空间
dxvk::war3::semantic 也纳入候选集，于是每个差分调用都会变成"引用实现 vs 模块实现"的
二义重载，测试根本无法编译——这不是语义差异，而是差分夹具的构造约束。因此差分测试
显式逐枚举值比对了模块类型与 legacy 类型（Shim.Source.* / Shim.Backing.* 共 17 项），
并逐字段比较 War3SemanticSceneFrameScore 的三个计数（Score.*）。

文件头部记录 provenance：git show ae890542d766:… 与迁移前 blob 的 SHA-256
3541DF5FE02EB9045B4A8499A1E7F2CBD2AB30DFF07896AFE1AAE43F1C847454；
新增门禁把该 SHA 与参考文件自身的 SHA-256 一并钉死，改参考文本即红。

---

## 4. 可执行等价测试

### 4.1 形态

- 目标：build32/src/d3d9/war3_device_semantic_predicates_test.exe
  （meson 名 warvk:war3_device_semantic_predicates，82 个 meson 测试之一，超时 30 s，实测 < 1 s）。
- 源：tests/war3_device_semantic_predicates_test.cpp + **真实**模块
  war3_device_semantic_predicates.cpp。
- 测试文件为模块调用的 device/注册表/钩子原语提供**宿主机替身**（host stubs）：
  可读内存范围表、SafeCopy、widget 身份缓存（ptr/handle）、RenderObjectRegistry、
  ShadowObjectRegistry、VisibleRenderableRegistry、VisibleRenderablePartLayerQueryCache、
  语义场景开关、RenderObjectIdentitySnapshot 的构造函数与判定。
- **引用实现与模块实现共用同一份替身**，因此差分隔离出的正是 M1 搬动的那部分逻辑。
- 测试文件与 legacy 参考都**不**包含 d3d9_device.cpp；不需要 Vulkan/D3D9 设备。

### 4.2 覆盖（同一输入 → 两个实现必须给出相同结果）

| 判定 | 输入域 | 检查点 |
| --- | --- | ---: |
| War3SemanticByteSwapU32 | 8x256 槽 + 边界（0/全 1/0x80000000/0xFFFF0000…） | ~2.1k |
| War3SemanticFourCcHasPrefix | 8 个 rawcode x 4 组前缀 + 65,536 个构造值 | 65.6k |
| War3SemanticFourCcEqualEitherOrder | 6x6 小域穷举（含 0/0、交换序） | 36 |
| War3SemanticRawcodeLooksStaticWorldCaster | 8 个 rawcode + 200,000 随机 | 200k |
| War3IsSemanticUnitObject | 全部 6 个 ObjectKind | 6 |
| War3SemanticPacketHasStableUnitResource | 5 个资源位的 2^5 穷举 | 32 |
| War3SemanticDirectSelectionKey | 10 个身份位的 2^10 穷举 + contract 通道 2^3 + 全 1 上界 + 20,000 随机 packet（含 outSource 与 nullptr 形态） | ~22k |
| War3SemanticReadUnitCore | 4,000 例：可读/不可读 x rawcode x Building 标志 x sprite 有/无（真实构造的 CUnit 缓冲 + 可读范围注册） | 16k |
| War3SemanticPacketHasConsistentUnitCore | 同上（含 thread-local 校验缓存） | 4k |
| War3SemanticDirectPacketMatchesMainWorldVisibleRecord | renderablePart 有/无 x 队列 2 x group 4 x 身份位 2^4 + 20,000 随机 | ~20.3k |
| War3SemanticDirectPacketHasMainWorldVisibleBacking | 同上 + 注册表命中/未命中 | ~40.6k |
| War3PacketIsPathBlocker | 20,000 随机 packet（rawcode / jHandle / widget 直读三通道，含 magic 对错、rawcode 空、不可读指针） | 20k |
| War3ShadowIsLosBlockerByJHandleFallback | 6,000 例（registry + widget 缓存各自的命中/miss） | 6k |
| War3ShadowIsLosBlockerByWidgetPtr | 同上 + 40 帧跨帧负缓存 TTL 序列 + frame=0 + nullptr | ~6k |
| War3ShouldSubmitSemanticPacket | 20,000 packet x 6 ObjectKind x unitsOnly 2（含**拒绝计数增量**与 **widget 写穿缓存终态**比对） | 240k |
| War3HasSemanticDynamicUnitEvidence / War3IsEligibleSemanticDynamicUnit | 30,000 随机 packet x 2 x 6 | 420k |
| War3ShouldSubmitSemanticPacketFast / War3LooksSubmitEligibleForScoringFast | 同上 | 420k |
| War3SemanticMaterialIsSafeOpaqueWorldCaster | 30,000 随机 packet（alphaMode 3 态 x payload 有无 x 队列 x blendOrDrawMode） | 30k |
| War3IsEligibleSemanticStaticWorldCaster | 30,000 x geoset 2 x geometry 2 x kind 6 | 720k |
| War3ResolveSemanticPacketObjectKind(Fast) | 30,000 随机 renderable/packet（三个注册表可命中） | 60k |
| War3ScoreSemanticSceneFrame / War3ShouldPreferSemanticSceneFrame | 4,000 帧对（帧号 0/非 0、空 draws、同 shared_ptr、null 帧）+ 评分逐字段 | ~48k |
| War3SemanticDirectRecordSelectionKey | 20,000 record（renderablePart 有无、缓存分支与直查分支、outVisibleHint 内容） | 60k |
| 类型 shim 等值 | 17 个枚举值 + 3 个 score 字段 | 20 |

**实测结果（原始输出）**：

    device semantic predicates equivalence: 3098194/3098194 checks passed
    RUN_EXIT=0

### 4.3 --probe：运行时配置 getter 的独立对照

函数内 static 使 env getter 一进程只读一次 env，因此单进程无法覆盖回退分支。
--probe 模式在同一进程内同时打印模块实现与 legacy 实现的值，门禁用 6 组 env 场景
分别起进程验证"两侧一致"并对照**独立登记的期望值**（覆盖 0 / 上界 / clamp / strtoul base-0 /
八进制 / 空值 / 无数字 / 溢出 / 负号）：

| 场景 | 覆盖点（节选） | 结果 |
| --- | --- | --- |
| default | 全部默认值：lease frames=120、fill margin=8、min records=16、TTL=8、RejectAlphaBlend=1、RejectUnsafeAlpha=0 | module==legacy==期望 |
| all-zero-rollback | max(1,0)=1、min(64,0)=0、clamp(0,1,120)=1、"abc"→fallback 7 | 同上 |
| upper-clamps | 100000 原样、margin 100→64、TTL 1000→120、"0x1F"→31 | 同上 |
| base0-and-sign | "08"（base 0 八进制，"8" 非法）→0、"2"→真、"0x10"→16 | 同上 |
| octal-and-empty | "010"→8、空字符串→fallback 1、"12abc"→12 | 同上 |
| unsigned-negation | "-1"→0xFFFFFFFF（32 位 unsigned long 取反）、lease frames max(1,0xFFFFFFFF) | 同上 |

此外门禁以两组 env 矩阵**整体重跑**差分测试（default 与 rollback-branches：
VALIDATE_UNIT_CORE=0、REJECT_UNSAFE_ALPHA_CASTER=1、REJECT_ALPHA_BLEND_CASTER=0、
WIDGET_PROBE_SAFE_COPY=0、WIDGET_NEGATIVE_FRAME_CACHE=0、OBJECT_GROUPED_SELECTION=0），
两组都必须 exit 0 且检查数 >= 3,000,000。

### 4.4 明确未覆盖（不得粉饰）

1. **替身不是生产实现**：本测试证明"同一输入 + 同一替身世界下两个实现一致"，
   **不证明**注册表/内存读取原语、VisibleRenderableRegistry 的查询语义本身正确。
2. **traceWorldWidgetProbe=true 分支未纳入差分**：该分支只多调用同一个 inline
   War3EnterShadowBuildContextPhase（trace 副作用在宿主机不可观测）。两侧调用的是
   同一 inline 函数、其后控制流与返回路径逐 token 相同，由 §3 的文本同一性覆盖；
   若将来该分支加入可观测副作用，本测试不会发现。
3. **函数内静态的 env 分支**：单进程只能覆盖一组 env；已用 6 组 probe + 2 组差分矩阵
   覆盖主要分支，但**不是** env 全组合穷举。
4. **device.cpp 侧调用点编排不在覆盖范围**（属 M4）；本测试不链接 device.cpp。
5. **不覆盖** GPU、Vulkan、实机画面与性能。

### 4.5 无法在宿主机链接的部分与替代证据

- 模块 .cpp 本身可在宿主机编译并链接（实测），因此 **26 个符号没有一个是
  "只能靠文本对照"**：全部有可执行差分证据。
- 真正无法在宿主机运行的是**被替身替换的生产原语**（注册表/内存/钩子）——它们由替身
  提供确定性输入，属于夹具边界，见 §4.4-1。

---

## 5. 变异验证（真跑：改 → 构建 → 跑 → 红 → 还原 → 复跑绿）

两条变异都在 Below Normal + -j2 下构建
（start /belownormal /b /wait cmd /c ninja -C build32 -j2），构建 exit 0。

### 5.1 变异 (a)：把已迁出判定改错（反转一个比较）

- 改动：war3_device_semantic_predicates.cpp 的 War3SemanticPacketHasConsistentUnitCore 中
  (unitFlags5C & UnitFlags5C::Building) == 0u → != 0u。
- 改动后源码 SHA-256：A6862232522B6DEE2713F6F5848247C21CBD2AF30D5873FC83908B7E3EF33A17
- 构建：NINJA_EXIT=0
- 差分测试（原始输出节选）：

      TEST_EXIT=1
      DIFF line 817 PacketHasConsistentUnitCore: module=1 legacy=0
      DIFF line 817 PacketHasConsistentUnitCore: module=0 legacy=1
      ...
      （DIFF 行合计 504）

- 等价证据门禁（原始输出）：

      GATE_EXIT=1
      差分测试 env 矩阵 default 失败 exit=1
      device semantic predicates equivalence: 3097690/3098194 checks passed
      DIFF line 817 PacketHasConsistentUnitCore: module=1 legacy=0

- **还原**：源码 SHA-256 回到 73C7D1A2185BE2FF77A80609CA6C39C1342ECFEC50F6D78ED0FD360AB1AACC0A
  （与改动前**逐字节相同**），NINJA_EXIT=0，差分测试
  3098194/3098194 checks passed、TEST_EXIT=0，门禁 GATE_EXIT=0。

### 5.2 变异 (b)：在 device.cpp 重新内联一个同类语义符号（职责回流）

- 改动：d3d9_device.cpp 在 using namespace dxvk::war3::semantic; 之后新增
  bool War3SemanticReflowedSelectionKeyProbeForGate() { return false; }
  （列 0 定义，属语义选择命名族）。
- 改动后 d3d9_device.cpp SHA-256：C4C9F8384264EE76BFBF66F38BE30D8EA3D1AC542358C7404D04809EF62C2BA1
- 构建：NINJA_EXIT=0（该改动能正常编译链接）
- 预算门禁（原始输出节选）：

      GATE_EXIT=1
      AssertionError: d3d9_device.cpp 新增了语义选择职责符号，违反迁移方向：
        War3SemanticReflowedSelectionKeyProbeForGate
      …

- **还原**：d3d9_device.cpp SHA-256 回到
  2736335B42FCF4947B728F2907A04FE9F63EC2E5C8683A887D0785432E499F1B（逐字节相同），
  NINJA_EXIT=0，预算门禁 GATE_EXIT=0，原始输出：
  "device semantic responsibility budget static checks passed（冻结符号 145；行首站点
  迁移前 154 -> 迁出后上限 124 -> 当前 124；已迁出 26 个符号；device.cpp 行首定义站点总数 591）"，
  ninja -C build32 -n = no work to do。

### 5.3 变异只证明"门禁/测试能抓这类回归"

两条变异分别覆盖：**单个判定的语义改动**（差分测试与证据门禁）与**职责回流**
（预算门禁）。它们**不**证明其他类型的回归（如未识别的命名绕过、替身原语错误）也会被抓；
预算门禁自己也在文档里声明了命名族之外的盲区。

---

## 6. 门禁原始输出（全部 Below Normal + -j2；构建后的最终复跑）

| 门禁 | 原始结果 |
| --- | --- |
| 构建 start /belownormal /b /wait cmd /c ninja -C build32 -j2 | NINJA_EXIT=0 |
| ninja -C build32 -n | ninja: no work to do. |
| 职责预算门禁 | EXIT 0：冻结符号 145；行首站点 迁移前 154 -> 迁出后上限 124 -> 当前 124；已迁出 26 个符号；device.cpp 行首定义站点总数 591 |
| **等价证据门禁（新增）** | EXIT 0：已迁出符号 26 个全部有 legacy 差分覆盖；legacy 参考 43452 B / SHA-256 2EA43F9782BE388E…；差分下限 3000000 断言；probe 场景 6 组 |
| 差分测试（默认 env） | device semantic predicates equivalence: 3098194/3098194 checks passed，RUN_EXIT=0 |
| 记录器 war3_palette_object_evidence_test | SUMMARY: 23 passed, 0 failed，PROC_EXIT=0 |
| 成本 war3_palette_object_evidence_cost_test | COST_VERDICT=PASS checks=38 failures=0，SUMMARY: all checks passed，PROC_EXIT=0 |
| 生命周期 war3_shadow_build_lifecycle_test | SUMMARY: war3_shadow_build_lifecycle_test 187/187 case(s) passed，PROC_EXIT=0 |
| 解析器静态 AutoTest/test_palette_object_evidence_analysis_static.py | Ran 94 tests ... OK，PROC_EXIT=0 |
| 通用根读方静态 AutoTest/test_analyze_frame_evidence.py | Ran 55 tests ... OK，PROC_EXIT=0 |
| 往返 py AutoTest/test_palette_object_wire_roundtrip.py <exe> | ROUNDTRIP_VERDICT=CERTIFIED_SPEC_SATISFIED，SPEC_UNMET=[]，PROC_EXIT=0 |
| meson test -C build32 --num-processes 2 | Ok: 82  Fail: 0，MESON_EXIT=0 |
| 全量静态 test_*_static.py | TOTAL=248 FAIL=0（上一轮 247；新增等价证据门禁 1 个） |

---

## 7. DLL 身份与"重建后 SHA 变化"的验证

- 最终产物：build32/src/d3d9/d3d9.dll，**36,271,456 B**，
  SHA-256 CB62253417CEC91ADC9A16F7D5BAC8E758370C62DFBFEB63D84906FF031750AE。
- 本轮**没有改动**任何影响该 DLL 的源码（war3_device_semantic_predicates.h/.cpp 与
  d3d9_device.cpp 的 SHA-256 与本文开始时一致），DLL 的代码内容即 M1 迁出后的同一实现。
- 本树早先已记录"同源重建后 DLL SHA 会变（PE 时间戳）"。本轮做了**受控验证**：
  在对象文件完全未变的情况下删掉 DLL 并重新链接两次，两次产物等长（36,271,456 B），
  全文件**仅 6 个字节、3 处**不同：

      0x88        ce d8 -> 13 d9
      0xd8        b3 3b -> 3d 3c
      0x1d68004   ce d8 -> 13 d9

  三处都是链接时间戳类字段（前两处的字节模式相同，末处位于文件尾部的调试目录区域）。
  因此：**源码逐字节相同的两次构建，DLL 差异仅为内嵌链接时间戳**——
  这也说明 CB622534… 与迁移前那次构建的 A0A51AF2BB9091B2C677DC34BEBDF76C02347049A89D0BB2AEF4C37518E52134
  （36,271,456 B，即现场部署版本）在**代码内容**上相同，仅链接时间戳不同。
- **未部署**：本轮没有覆盖 E:\Work\War3 下的任何文件（现场 DLL 仍为 A0A51AF2…），
  未启动游戏、未启动编辑器。

---

## 8. 未做到 / 不确定 / 明确的措辞边界

1. **M2/M3/M4 未开始**。M1 只迁出"纯判定与常量"。不能说"device.cpp 的语义职责已迁完"，
   也不能说架构重构已完成。
2. **差分是"同一输入域"证据，不是全输入证明**。输入域由枚举小域 + 固定种子随机组成
   （规模见 §4.2），未覆盖的输入组合仍然存在。
3. **替身世界是夹具**（§4.4-1）。注册表/内存读取原语本身未被证明。
4. **env 分支非穷举**（§4.4-3）。
5. **traceWorldWidgetProbe=true 未差分**（§4.4-2）。
6. **未实机、未部署、未提交、未晋升稳定**。本文不含任何画面、性能或稳定性结论。
7. 变异验证只覆盖两条回归类型（§5.3）。
8. 数字口径：差分检查点数是**测试自报**的检查次数（含部分同一输入的多字段对照），
   不是"输入组合数"；--probe 是 6 组 env 场景，不是 env 全空间。
9. 本文的对照表基于**本次工作树状态**；若将来 M2/M3/M4 改动 device.cpp 或模块，
   必须重新生成 legacy 参考与差分覆盖，不能沿用本文结论。
10. 观察（不属于 M1 改动）：预算门禁文件 AutoTest/test_device_semantic_responsibility_budget_static.py
   混用了 CRLF 与孤立 CR 行尾（按 LF 计 315 行，按 Python splitlines 计 459 行）。
   该门禁自身用 text.splitlines() 解析，行为不受影响；但任何按 "\n" 切行的外部工具
   都会看到合并后的行。新增的等价证据门禁因此不按行切分，而是直接在 FROZEN 表块内
   用正则抽取条目（实测与预算门禁一致：145 个冻结符号）。
