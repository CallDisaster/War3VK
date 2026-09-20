"""防止 War3 语义选择职责回流到 d3d9_device.cpp 的固定预算门禁（fail-closed）。

背景（2026-09-16 架构复审目标项 ⑤，M1 已落地）：
    d3d9_device.cpp 应只保留 D3D9 资源生命周期与 Present 安全点编排；
    “哪个对象/部件/调色板/几何是权威”的判定属语义选择职责，应迁往语义模块。
    迁移方案见 docs/plan/2026-09-16-device-semantic-responsibility-migration.md。

冻结口径（逐符号，2026-09-17 M1 迁移前后实测）：
    FROZEN[name] = (baseline, budget)
      baseline = 迁移前 device.cpp 中该符号的行首“定义/声明站点”数（迁移前预算）
      budget   = 迁移后允许的上限（<= baseline；只允许减少，增加即失败）
    站点口径 = 源文件第 0 列开始、且以 “SYMBOL(” 结尾签名的行。它同时覆盖函数定义与
    前置声明，因此“把定义挪走但保留同名声明”也会被计入而不是被忽略。

三条独立判定（任一失败即红）：
    1. 预算：每个冻结符号在 device.cpp 的行首站点数必须 <= budget。
    2. 迁出落地：站点数低于 baseline 的符号必须出现在 MODULE_HOME 中；否则是
       “改名绕过 / 直接删除”而不是迁出。
    3. 回流：device.cpp 中任何符合语义选择命名族的行首定义名都必须已在 FROZEN 内
       —— 新增同类符号、或把冻结符号改名成同类新名，都在此失败。

fail-closed：任一必需文件缺失、解析结果异常（符号总数过小、模块内看不到迁出符号）
都直接失败，不允许“解析不出符号就算通过”。

边界（明确不覆盖）：把语义判定改写成不匹配命名族、且不删除任何冻结符号的全新名字，
本门禁无法按名字识别；这类改动只能靠 code review。需要新增同类符号时，必须把符号加入
FROZEN（并说明为什么不能迁出）——该改动本身会在 diff 里可见。
"""

from pathlib import Path
import collections
import re


ROOT = Path(__file__).resolve().parents[1]
DEVICE_PATH = ROOT / "src/d3d9/d3d9_device.cpp"
MODULE_HOME_PATHS = (
    ROOT / "src/d3d9/war3/semantic/war3_device_semantic_predicates.h",
    ROOT / "src/d3d9/war3/semantic/war3_device_semantic_predicates.cpp",
    ROOT / "src/d3d9/war3/semantic/war3_live_palette_selection.h",
    ROOT / "src/d3d9/war3/semantic/war3_live_palette_selection.cpp",
    ROOT / "src/d3d9/war3/semantic/war3_palette_taxonomy_emission.h",
    ROOT / "src/d3d9/war3/semantic/war3_palette_taxonomy_emission.cpp",
    ROOT / "src/d3d9/war3/debug/war3_shadow_build_context_trace.h",
)

# (baseline, budget) —— 只允许减少。
FROZEN = {
    "War3BuildS1TerrainEarlyKey": (1, 1),
    "War3CanPromoteShadowPersistentGeometry": (1, 1),
    "War3CasterIsAnonymousSmallPathBlockerMarker": (1, 1),
    "War3ClassifyShadowReplayMode": (1, 1),
    "War3CoherentRealDomainCacheRuntime": (1, 1),
    "War3CoherentRealHintDomainRuntime": (1, 1),
    "War3ComputeTerrainBoundsContentKey": (1, 1),
    "War3CurrentDrawGenerationIndexSliceCacheRuntime": (1, 1),
    "War3CurrentDrawGroupRangeCacheRuntime": (1, 1),
    "War3CurrentDrawVisibleIndexSliceCacheRuntime": (1, 1),
    "War3DecodeRuntimePoseMatrix48": (1, 1),
    "War3DrawTimeCacheIteratorReuseRuntime": (1, 1),
    "War3DrawTimeCacheIteratorReuseVerifyRuntime": (1, 1),
    "War3DrawTimeCurrentFrameGeometryRuntime": (1, 1),
    "War3DrawTimeSourceFingerprintReuseRuntime": (1, 1),
    "War3DrawTimeVBCacheRuntime": (1, 1),
    "War3ExactIndexedFreezeTrimRuntime": (1, 1),
    "War3GpuSkinResolveNativeCpuRewriteOutputProof": (1, 1),
    "War3IsEligibleSemanticDynamicUnit": (1, 0),
    "War3IsEligibleSemanticStaticWorldCaster": (1, 0),
    "War3KeepS1TerrainLegacyCaptureRuntime": (1, 1),
    "War3LegacyDrawIsPathBlockerGeometryMarkerCandidate": (1, 1),
    "War3LegacyMaterialSignatureScopeRuntime": (1, 1),
    "War3LegacyPerDrawSemanticScopesRuntime": (1, 1),
    "War3MakeDrawTimeVBCacheKey": (1, 1),
    "War3MakeShadowMetadataKey": (1, 1),
    "War3NativeHintProducerlessSkipRuntime": (1, 1),
    "War3PacketIsPathBlocker": (2, 0),
    "War3PopulateSubmitPermutationVerifierAssertRuntime": (1, 1),
    "War3PopulateSubmitPermutationVerifierRuntime": (1, 1),
    "War3PopulateSubmitPermutationViewRuntime": (1, 1),
    "War3ProducerClaimObserveKey": (1, 1),
    "War3ProducerClaimObserveModeRuntime": (1, 1),
    "War3ProducerClaimObserveObjectKey": (1, 1),
    "War3ResolveCurrentDrawCaptureBinding": (1, 1),
    "War3ResolveLivePoseRuntimeAlias": (1, 1),
    "War3ResolveSemanticPacketObjectKind": (2, 0),
    "War3ResolveSemanticPacketObjectKindFast": (1, 0),
    "War3S1EarlyFallbackBackingRuntime": (1, 1),
    "War3S1ForceIdentityWorldRuntime": (1, 1),
    "War3S1PersistentBorrowStaticRuntime": (1, 1),
    "War3S1PersistentUnstableSourceRuntime": (1, 1),
    "War3S1TerrainCaptureDueRuntime": (1, 1),
    "War3S1TerrainCapturePeriodRuntime": (1, 1),
    "War3S1TerrainPersistentGeometryRuntime": (1, 1),
    "War3ScoreSemanticSceneFrame": (1, 0),
    "War3SemanticAllowCanonicalSinglePrimitiveFullIndexRuntime": (1, 1),
    "War3SemanticBootstrapSupplementedBuildRuntime": (1, 1),
    "War3SemanticBoundsRadiusForObjectKind": (1, 1),
    "War3SemanticBypassInlineRegistryPublishRuntime": (1, 1),
    "War3SemanticCompactWorkTableModeRuntime": (1, 1),
    "War3SemanticContractCapturePeriodRuntime": (1, 1),
    "War3SemanticDirectCurrentDrawRecordCapRuntime": (1, 1),
    "War3SemanticDirectCurrentDrawScanCapRuntime": (1, 1),
    "War3SemanticDirectExplicitBlendResolveRuntime": (1, 1),
    "War3SemanticDirectOnlyRuntime": (1, 1),
    "War3SemanticDirectOwnerScanRuntime": (1, 1),
    "War3SemanticDirectPartPacketLeaseRuntime": (1, 1),
    "War3SemanticDirectRecordPriorityScore": (1, 1),
    "War3SemanticDirectRecordSelectionKey": (1, 0),
    "War3SemanticDirectSelectionKey": (1, 0),
    "War3SemanticDirectStaticSupplementRuntime": (1, 1),
    "War3SemanticDrawTimeDirectProducerRuntime": (1, 1),
    "War3SemanticDrawTimeFastAppendRuntime": (1, 1),
    "War3SemanticDrawTimePoseRuntime": (1, 1),
    "War3SemanticDrawTimePrebuildBypassRuntime": (1, 1),
    "War3SemanticDynamicEvidenceStatsRuntime": (1, 1),
    "War3SemanticFastAppendRegionCacheRuntime": (1, 1),
    "War3SemanticFastAppendStatsReuseRuntime": (1, 1),
    "War3SemanticFastAppendStatsReuseVerifierRuntime": (1, 1),
    "War3SemanticGenericAppendStatsReuseRuntime": (1, 1),
    "War3SemanticGenericAppendStatsReuseVerifierRuntime": (1, 1),
    "War3SemanticLivePaletteAllowCModelFallbackRuntime": (1, 1),
    "War3SemanticLivePaletteRefreshRuntime": (1, 1),
    "War3SemanticLivePaletteSafeCopyRuntime": (1, 1),
    "War3SemanticMaterialSignatureCacheRuntime": (1, 1),
    "War3SemanticObjectFirstSnapshotRuntime": (1, 1),
    "War3SemanticObjectGroupedSelectionRuntime": (1, 0),
    "War3SemanticPaletteDiagnosticsRuntime": (1, 1),
    "War3SemanticPaletteInPlaceAppendRuntime": (1, 1),
    "War3SemanticPoseOnlyCapturePeriodRuntime": (1, 1),
    "War3SemanticPublishRegistriesBeforeSceneRuntime": (1, 1),
    "War3SemanticRejectAlphaBlendCasterRuntime": (1, 0),
    "War3SemanticRejectUnsafeAlphaCasterRuntime": (1, 0),
    "War3SemanticRequireAuthoritativeSkinnedRuntime": (1, 1),
    "War3SemanticRequireDirectUnitVisibleBackingRuntime": (1, 1),
    "War3SemanticRequireVisibleIndexSliceForSkinnedRuntime": (1, 1),
    "War3SemanticShadowManifestCModelPoseRestoreRuntime": (1, 1),
    "War3SemanticShadowManifestCoreEpochPlannerRuntime": (1, 1),
    "War3SemanticShadowManifestCoreStalePoseOneFrameRestoreRuntime": (1, 1),
    "War3SemanticShadowManifestDeferProvisionalPartsRuntime": (1, 1),
    "War3SemanticShadowManifestGeometryCacheFramesRuntime": (1, 1),
    "War3SemanticShadowManifestLeasePaletteRefreshRuntime": (1, 1),
    "War3SemanticSteadySupplementedBuildRuntime": (1, 1),
    "War3SemanticStickyPartSelectionMinRecordsRuntime": (1, 0),
    "War3SemanticStickyPartSelectionRuntime": (1, 0),
    "War3SemanticStickySelectionBroadLeasePreferenceRuntime": (1, 0),
    "War3SemanticStickySelectionFillMarginRuntime": (1, 0),
    "War3SemanticStickySelectionFillRuntime": (1, 0),
    "War3SemanticStickySelectionLeaseFramesRuntime": (1, 0),
    "War3SemanticStickySelectionLeaseRuntime": (1, 0),
    "War3SemanticSubmitDrawCapRuntime": (1, 1),
    "War3SemanticValidateUnitCoreRuntime": (1, 0),
    "War3ShadowCaptureGateBreakdownRuntime": (1, 1),
    "War3ShadowCapturePostBreakdownRuntime": (1, 1),
    "War3ShadowDrawTimeCaptureBreakdownRuntime": (1, 1),
    "War3ShadowIsLosBlocker": (5, 5),
    "War3ShadowIsLosBlockerByJHandleFallback": (2, 0),
    "War3ShadowIsLosBlockerByWidgetPtr": (1, 0),
    "War3ShadowMetadataAlphaRuntime": (1, 1),
    "War3ShadowMetadataBlockerRuntime": (1, 1),
    "War3ShadowMetadataCaptureRuntime": (1, 1),
    "War3ShouldDrawDebugOverlay": (1, 1),
    "War3ShouldDrawOutline": (1, 1),
    "War3ShouldOverridePostProcessMaterial": (1, 1),
    "War3ShouldOverrideWorldMaterial": (1, 1),
    "War3ShouldPreferSemanticSceneFrame": (1, 0),
    "War3ShouldSubmitSemanticPacket": (2, 0),
    "War3ShouldSubmitSemanticPacketFast": (1, 0),
    "War3SmallPathBlockerMarkerGeometryFits": (1, 1),
    "War3Stage11AllocationObserverRuntime": (1, 1),
    "War3Stage11DirectStaticSourceRuntime": (1, 1),
    "War3Stage11DirectUploadSourceRuntime": (1, 1),
    "War3Stage13ComponentDiagnosticsRuntime": (1, 1),
    "War3Stage13LateDescriptorCacheRuntime": (1, 1),
    "War3Stage13LateDescriptorSampleCountRuntime": (1, 1),
    "War3Stage13LateFullIndexFingerprintRuntime": (1, 1),
    "War3Stage13SortUniqueReadsRuntime": (1, 1),
    "War3Stage13SourceGenerationVerifyRuntime": (1, 1),
    "War3Stage13StaticRetentionCapRuntime": (1, 1),
    "War3Stage13StaticRetentionFramesRuntime": (1, 1),
    "War3Stage13StaticRetentionRuntime": (1, 1),
    "War3Stage13UniqueSemanticCacheRuntime": (1, 1),
    "War3TryBuildCurrentDrawRecordMaterialSignature": (1, 1),
    "War3TryBuildLiveRuntimeGroupPalette": (2, 2),
    "War3TryBuildShadowPacketFromCurrentDrawRecord": (1, 1),
    "War3TryFindShadowPersistentGeometry": (1, 1),
    "War3TryPopulateDirectCurrentDrawGrouped": (1, 1),
    "War3TryPopulateDrawTimeSemanticProducer": (1, 1),
    "War3TryPopulateSemanticShadowScene": (1, 1),
    "War3TryReadRuntimePoseArray": (1, 1),
    "War3TryResolveNativeShadowHint": (1, 1),
    "War3VisibleRenderableMatchesDrawTimeKey": (1, 1),
    "War3WidgetNegativeFrameCacheRuntime": (1, 0),
    "War3WidgetProbeSafeCopyRuntime": (1, 0),
}

# M2-1（2026-09-18）：live palette 选择链 8 个符号迁往
# src/d3d9/war3/semantic/war3_live_palette_selection.*。
# 这 8 条登记在独立的 FROZEN_M2_1 再 update 进 FROZEN，而不是直接写进上面的
# FROZEN 块：M1 等价门禁（AutoTest/test_device_semantic_predicates_equivalence_static.py，
# 本轮一个字节不改）按文本解析上面那个 FROZEN 块、并以 budget<baseline 推导
# "已迁出集合"对照它自己硬编码的 26 个 COVERED；M2-1 的 8 个符号若带
# budget<baseline 出现在该块，会被 M1 门禁误判为"已迁出但无 M1 等价证据"。
# 其中 7 个名字与上面块里的 (1, 1) 条目重复是刻意的：dict.update 后本表的
# budget=0 生效（判定 1/2 对 8 个符号照常咬合，行首站点只允许为 0），而
# M1 门禁只看到上面块里的 (1, 1)，继续只钉 M1 自己的 26 个符号。
# War3SemanticHashMatrixPalette 的 baseline=2：站点口径同时覆盖定义与前置
# 声明（迁移前 device.cpp :1213 前置声明 + :7426 定义），与
# War3PacketIsPathBlocker (2, 0) 同一先例。
FROZEN_M2_1 = {
    "War3SemanticHashMatrixPalette": (2, 0),
    "War3DecodeRuntimePoseMatrix48": (1, 0),
    "War3TryReadRuntimePoseArray": (1, 0),
    "War3ResolveLivePoseRuntimeAlias": (1, 0),
    "War3SemanticLivePaletteSafeCopyRuntime": (1, 0),
    "War3SemanticLivePaletteRefreshRuntime": (1, 0),
    "War3SemanticLivePaletteAllowCModelFallbackRuntime": (1, 0),
    "War3SemanticPaletteDiagnosticsRuntime": (1, 0),
}
FROZEN.update(FROZEN_M2_1)

# M2-2（2026-09-18）：选择链本体 War3TryBuildLiveRuntimeGroupPalette（含
# resolvePaletteSlotIndex lambda 与其 thread_local 缓存）迁往
# src/d3d9/war3/semantic/war3_live_palette_selection.*；Gap A 计数器
# g_devicePaletteSlotCache* 按 M1 D 类先例随迁（变量定义不匹配行首函数站点口径，
# 不占 FROZEN 行）。War3SemanticPaletteSource / War3LivePaletteBuild* 类型与枚举
# 同样随迁——不是行首函数/声明站点、不匹配命名族 regex，不占 FROZEN 行
#（等价记录 docs/plan/2026-09-18-m2-2-migration-equivalence-record.md 有显式说明）。
# 与 FROZEN_M2_1 同构：独立成表再 update，避免 M1 等价门禁（只解析上面原始
# FROZEN 块、以 budget<baseline 推导 M1 迁出集合）把本符号误判为"M1 迁出但无
# M1 等价证据"。baseline=2：迁移前 device.cpp 的行首站点同时覆盖前向声明与定义
#（与 War3PacketIsPathBlocker (2, 0)、M2-1 War3SemanticHashMatrixPalette (2, 0)
# 同一先例）。
FROZEN_M2_2 = {
    "War3TryBuildLiveRuntimeGroupPalette": (2, 0),
}
FROZEN.update(FROZEN_M2_2)

# M2-3（2026-09-18）：motion / churn 诊断三函数
# （War3NoteLivePaletteMotion / War3NoteDrawTimePoseMotion /
#  War3NoteSubmittedPaletteMotion）连同各自 Entry 结构迁往
# src/d3d9/war3/semantic/war3_live_palette_selection.{h,cpp}。
# 这三个符号**不匹配**上面的 SEMANTIC_FAMILY_RE 命名族（名字里没有
# Resolve/Select/Runtime/Key/... 任何后缀），因此判定 3 看不见它们：
# 不登记就是漏检——把任一函数重新内联回 device.cpp 不会被任何一条判定
# 咬住。本表把它们显式冻结成 budget=0，使"职责回流"在判定 1 上被点名。
# baseline=1：迁移前 device.cpp 的行首站点只有定义本身（实测口径 = 本文件
# declaration_sites 的同一 regex 与同一行首规则；三个符号均无前置声明，
# Entry 结构以 '{' 结尾不匹配 DEF_RE，不占站点）。
# 与 FROZEN_M2_1/FROZEN_M2_2 同构：独立成表再 update，避免 M1 等价门禁
# （只解析上面原始 FROZEN 块、以 budget<baseline 推导 M1 迁出集合）把这三个
# 符号误判为"M1 迁出但无 M1 等价证据"。
FROZEN_M2_3 = {
    "War3NoteLivePaletteMotion": (1, 0),
    "War3NoteDrawTimePoseMotion": (1, 0),
    "War3NoteSubmittedPaletteMotion": (1, 0),
}
FROZEN.update(FROZEN_M2_3)

# M2-5（2026-09-18）：skinned palette taxonomy 发射块（d3d9_device.cpp
# :21202-21580 的 379 行，含 34 个 stats 字段与 3 张跨帧 thread_local 探针表）
# 迁往 src/d3d9/war3/semantic/war3_palette_taxonomy_emission.{h,cpp} 的
# War3EmitSemanticPaletteTaxonomy。
# 该符号**不匹配**上面的 SEMANTIC_FAMILY_RE 命名族（名字里没有
# Resolve/Select/Runtime/Key/... 任何后缀），因此判定 3 看不见它；不登记就是
# 漏检——把该函数重新内联回 device.cpp 不会被任何一条判定咬住。
# baseline=0 是**实测值**而不是占位：迁移前 device.cpp 里既不存在这个符号名，
# 该块本身也不含任何第 0 列起始的 DEF_RE 站点（用本文件的 declaration_sites()
# 同一 regex 与同一行首规则扫过 pre-M2-5 快照的 :21202-21580 实测为空）。
# 因此 budget=0：device.cpp 一旦出现该行首定义，判定 1 立即点名。
# 与 FROZEN_M2_1/M2_2/M2_3 同构：独立成表再 update，避免 M1 等价门禁
# （只解析上面原始 FROZEN 块、以 budget<baseline 推导 M1 迁出集合）把它误判为
# "M1 迁出但无 M1 等价证据"。
FROZEN_M2_5 = {
    "War3EmitSemanticPaletteTaxonomy": (0, 0),
}
FROZEN.update(FROZEN_M2_5)

# M2-4（2026-09-18）：调色板侧余项（env getter / compose-policy 判定 / 纯查表）
# 迁往 src/d3d9/war3/semantic/war3_live_palette_selection.{h,cpp}：
#   A1 War3SemanticPaletteInPlaceAppendRuntime  (env getter, inline)
#   A2 War3SemanticDrawTimePoseRuntime          (env getter)
#   A6 War3SemanticTranslationFinite            (A4 supporting 纯判定)
#   A7 War3SemanticTranslationDistanceSq        (A4 supporting 纯计算)
#   A3 War3SemanticPaletteStorageReadable       (A4 supporting 纯判定)
#   A4 War3SemanticPaletteLooksModelLocal ×2    (compose-policy 判定本体)
#   A8 War3SemanticBoundsRadiusForObjectKind    (A4 依赖的纯查表；另 6 个调用点
#       属 bounds 族，文本未改，仍经同一 using-directive 解析到模块)
#   A5 War3SemanticHashMatrix4                  (HashMatrixPalette 的姊妹)
# 其中 A3/A4/A5 与 M2-3 的 War3Note*Motion 同类：**不匹配** SEMANTIC_FAMILY_RE
# 命名族（无 Resolve/Select/Runtime/Key/... 后缀），判定 3 对它们盲；
# 不登记就无法捕到"重新内联回 device.cpp"。A1/A2/A8 虽匹配命名族，也一并
# 显式登记以钉死 budget=0。
# baseline 是**实测值**（不是占位）：用本文件同一 DEF_RE 与同一"第 0 列起始、
# 非注释行"规则扫 pre-M2-4 快照（%TEMP%\device_pre_m2_4.cpp，2,254,826 B /
# 7391F307…A0844）实测得到 1/1/1/2/1/1/1/1 个行首站点（A4 两个重载各 1）。
# 与 FROZEN_M2_1/M2_2/M2_3/M2_5 同构：独立成表再 update，避免 M1 等价门禁
# （只解析上面原始 FROZEN 块、以 budget<baseline 推导 M1 迁出集合）把本表符号
# 误判为"M1 迁出但无 M1 等价证据"。
FROZEN_M2_4 = {
    "War3SemanticPaletteInPlaceAppendRuntime": (1, 0),
    "War3SemanticDrawTimePoseRuntime": (1, 0),
    "War3SemanticTranslationFinite": (1, 0),
    "War3SemanticTranslationDistanceSq": (1, 0),
    "War3SemanticPaletteStorageReadable": (1, 0),
    "War3SemanticPaletteLooksModelLocal": (2, 0),
    "War3SemanticBoundsRadiusForObjectKind": (1, 0),
    "War3SemanticHashMatrix4": (1, 0),
}
FROZEN.update(FROZEN_M2_4)

# M2-5B（2026-09-18）：M2-5 余项 B4——每帧 submitted skinned palette 聚合块
# （pre device.cpp :22200-22244 的 45 行，7 个 stats 字段 + 顺序敏感滚动 FNV-1a）
# 迁往 src/d3d9/war3/semantic/war3_palette_submitted_aggregation.{h,cpp} 的
# War3AggregateSubmittedSkinnedPalette。
# 该符号**不匹配**上面的 SEMANTIC_FAMILY_RE 命名族（名字里没有
# Resolve/Select/Runtime/Key/... 任何后缀），因此判定 3 看不见它；不登记就是漏检
# ——把该函数重新内联回 device.cpp 不会被判定 3 咬住。
# baseline=0 是**实测值**而不是占位：迁移前 device.cpp 全文出现该名字 0 次，且该块
# 45 行全部处于缩进层、不含任何第 0 列起始的 DEF_RE 站点（用本文件同一
# declaration_sites() regex 与同一行首规则扫 pre-M2-5B 快照 :22200-22244 实测为空）。
# 因此 budget=0：device.cpp 一旦出现该行首定义，判定 1 立即点名。
# 与 FROZEN_M2_1/M2_2/M2_3/M2_5/M2_4 同构：独立成表再 update，避免 M1 等价门禁
# （只解析上面原始 FROZEN 块、以 budget<baseline 推导 M1 迁出集合）把它误判为
# "M1 迁出但无 M1 等价证据"。
FROZEN_M2_5B = {
    "War3AggregateSubmittedSkinnedPalette": (0, 0),
}
FROZEN.update(FROZEN_M2_5B)

# A9（2026-09-18 死代码裁定）：inventory A9 War3SemanticBuildWorldPaletteIfNeeded
# 是"0 调用"的死代码，但其正文是 A4 compose-policy 所服务的那条
# model-local -> world-space 组合合同；裁定为**迁移而非删除**，正文逐字节迁往
# src/d3d9/war3/semantic/war3_live_palette_selection.{h,cpp}（等价证据见
# AutoTest/test_war3_palette_a9_migration_equivalence_static.py）。
# 该符号**不匹配**上面的 SEMANTIC_FAMILY_RE 命名族（名字里没有
# Resolve/Select/Runtime/Key/... 任何后缀），因此判定 3 看不见它；不登记就是
# 漏检——把该函数重新内联回 device.cpp 不会被判定 3 咬住。
# baseline=0 是**实测值**而不是占位：迁移前 device.cpp 里的定义行以
# "[[maybe_unused]] void ..." 开头，本文件同一 DEF_RE 要求第 0 列以标识符
# （或 inline/static/constexpr/virtual/explicit 之一）起始，[[maybe_unused]]
# 前缀不匹配 ⇒ 实测行首站点 0（用本文件同一 DEF_RE 与同一行首规则扫
# pre-A9 快照 %TEMP%\device_pre_a9.cpp 实测为 0）。
# 因此 budget=0：device.cpp 一旦出现该符号的行首定义（例如去掉 [[maybe_unused]]
# 前缀后回流），判定 1 立即点名。
# 与 FROZEN_M2_1/M2_2/M2_3/M2_5/M2_4/M2_5B 同构：独立成表再 update，避免 M1
# 等价门禁（只解析上面原始 FROZEN 块、以 budget<baseline 推导 M1 迁出集合）
# 把它误判为"M1 迁出但无 M1 等价证据"。
FROZEN_A9 = {
    "War3SemanticBuildWorldPaletteIfNeeded": (0, 0),
}
FROZEN.update(FROZEN_A9)

DEF_RE = re.compile(
    r"^(?:inline\s+|static\s+|constexpr\s+|virtual\s+|explicit\s+)*"
    r"[A-Za-z_][A-Za-z0-9_:<>,*&\s\[\]]*\b([A-Za-z_][A-Za-z0-9_]*)\s*\("
)
SKIP_NAMES = {
    "if", "for", "while", "switch", "return", "sizeof", "static_assert",
    "else", "catch", "do", "case", "defined",
}
# 语义选择命名族（与迁移清册同类）。
SEMANTIC_FAMILY_RE = re.compile(
    r"^(?:War3|IsLosBlocker).*"
    r"(?:Resolve|Select|Choose|Decide|Classify|Score|Should|TryBuild|TryFind|"
    r"TryPopulate|Promote|CanPromote|IsEligible|Runtime|SelectionKey|Key|"
    r"ObjectKind|PathBlocker|LosBlocker)"
)

MIN_DEVICE_SITES = 400
MIN_FROZEN_SYMBOLS = 100
MIN_MODULE_SYMBOLS = 25


def declaration_sites(text):
    """按行首（第 0 列）定义/声明站点计数；缩进的调用点不计入。"""
    sites = collections.Counter()
    for line in text.splitlines():
        if not line or line[:1] in (" ", "\t"):
            continue
        if line.startswith(("//", "#", "/*", "*")):
            continue
        match = DEF_RE.match(line)
        if not match:
            continue
        name = match.group(1)
        if name in SKIP_NAMES:
            continue
        sites[name] += 1
    return sites


for path in (DEVICE_PATH,) + MODULE_HOME_PATHS:
    if not path.is_file():
        raise AssertionError(f"fail-closed: 缺少必需文件 {path}")

if len(FROZEN) < MIN_FROZEN_SYMBOLS:
    raise AssertionError(
        f"fail-closed: FROZEN 只有 {len(FROZEN)} 个符号，冻结集合异常"
    )

device_sites = declaration_sites(DEVICE_PATH.read_text(encoding="utf-8"))
if sum(device_sites.values()) < MIN_DEVICE_SITES:
    raise AssertionError(
        "fail-closed: 无法从 d3d9_device.cpp 解析出足够的定义站点"
        f"（{sum(device_sites.values())} < {MIN_DEVICE_SITES}）"
    )

module_text = "\n".join(
    path.read_text(encoding="utf-8") for path in MODULE_HOME_PATHS
)
module_symbols = {
    name for name, (baseline, _budget) in FROZEN.items()
    if re.search(r"\b" + re.escape(name) + r"\b", module_text)
}
if len(module_symbols) < MIN_MODULE_SYMBOLS:
    raise AssertionError(
        "fail-closed: 语义模块内看不到足够的迁出符号"
        f"（{len(module_symbols)} < {MIN_MODULE_SYMBOLS}）"
    )

# 判定 1：预算（只允许减少）。
over_budget = {
    name: (device_sites.get(name, 0), budget)
    for name, (_baseline, budget) in FROZEN.items()
    if device_sites.get(name, 0) > budget
}
if over_budget:
    detail = "\n  ".join(
        f"{name}: device.cpp={found} > budget={budget}"
        for name, (found, budget) in sorted(over_budget.items())
    )
    raise AssertionError(
        "d3d9_device.cpp 中语义选择职责符号超过冻结预算（职责回流）：\n  " + detail
    )

# 判定 2：迁出落地（减少必须落到语义模块，否则是改名绕过）。
missing_home = sorted(
    name for name, (baseline, _budget) in FROZEN.items()
    if device_sites.get(name, 0) < baseline and name not in module_symbols
)
if missing_home:
    raise AssertionError(
        "以下符号在 d3d9_device.cpp 中减少了，但没有出现在语义模块中"
        "（改名绕过 / 直接删除），违反迁移方向：\n  "
        + "\n  ".join(missing_home)
        + "\n请把它们真正迁到 src/d3d9/war3/semantic/（或把模块里的新名字写回 FROZEN）。"
    )

# 判定 3：回流（新增同类符号 / 同类改名）。
added = sorted(
    name for name in device_sites
    if SEMANTIC_FAMILY_RE.match(name) and name not in FROZEN
)
if added:
    raise AssertionError(
        "d3d9_device.cpp 新增了语义选择职责符号，违反迁移方向：\n  "
        + "\n  ".join(added)
        + "\n请把该职责放到语义模块；确有理由留在 device 时，必须把符号加入 "
        "AutoTest/test_device_semantic_responsibility_budget_static.py 的 FROZEN "
        "（写清 baseline=0、budget=实际值）并在同一改动说明理由。"
    )

baseline_total = sum(baseline for baseline, _budget in FROZEN.values())
budget_total = sum(budget for _baseline, budget in FROZEN.values())
current_total = sum(device_sites.get(name, 0) for name in FROZEN)
migrated = sorted(
    name for name, (baseline, _budget) in FROZEN.items()
    if device_sites.get(name, 0) < baseline
)

print(
    "device semantic responsibility budget static checks passed"
    f"（冻结符号 {len(FROZEN)}；行首站点 迁移前 {baseline_total} -> 迁出后上限 "
    f"{budget_total} -> 当前 {current_total}；已迁出 {len(migrated)} 个符号；"
    f"device.cpp 行首定义站点总数 {sum(device_sites.values())}）"
)
