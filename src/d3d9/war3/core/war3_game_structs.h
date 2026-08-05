// war3_game_structs.h - War3 游戏内部结构定义与常量
// 整合了 d3d9_war3_structs.h 的结构体定义和偏移常量
// 完整的游戏结构定义请参考 jass/war3_game_struct.h
//
// Important:
// - This header is a shared ABI/offset surface, not a catalog of every
//   RTTI-confirmed Blizzard class.
// - Types imported from jass/war3_game_struct.h may be a mix of:
//   1) RTTI-confirmed engine/runtime classes, and
//   2) stable analysis-only records used to name recovered layouts.
// - Unresolved descriptors/records (for example the current
//   MeshLayerStateRecord + 0x1C remap/span blocker) must not be promoted here
//   as if they already had confirmed RTTI names.

#pragma once

#include <cstdint>
#include <cstddef>

#include "jass/war3_game_struct.h"

namespace dxvk::war3 {

// ============================================================================
// 结构体定义（原 d3d9_war3_structs.h）
// ============================================================================

// 使用完整结构定义（来源：jass/war3_game_struct.h）
//
// Top-level aliases below should stay limited to RTTI-backed or otherwise
// long-stable engine object types that we intentionally treat as object-level
// public ABI.
using ::HashGroup;
using ::CWidget;
using ::CUnit;
using ::CSprite;
using ::CSpriteAnimRequest;
using ::CGeoset;
using ::CGeosetData;
using ::CModel;
using ::CModelData;
using ::GeosetPrimitiveRecord;
using ::GeosetUvLayerVec2ArrayRecord;
using ::CModelGeosetBindingRecord;
using ::CModelAnimController;
using ::CAnimComplex;
using ::CAnimSequenceProvider;
using ::AnimationTimeState;
using ::AnimationSequenceData;
using ::CEffect;
using ::JassHandleNode;
using ::JassHandleTable;
using ::CGameState;
using ::CGameWar3;

// Stable layout records that are actively used by the renderer/research path,
// but are not currently promoted as RTTI-confirmed Blizzard classes.
namespace analysis {
using ::SceneNode;
using ::SceneNodeTintRecord;
using ::GxStagePresetRecord;
using ::RenderOverridePresetChannelDataRecord;
using ::RenderOverridePresetChannelResolverRecord;
using ::RenderOverrideTransformAuxRecord;
using ::RenderOverrideDependencyGateOutputRecord;
using ::RenderOverrideCompactScalarOutputEntry;
using ::RenderOverrideGraphOutputBundle;
using ::RenderStagePresetOverrideNode;
using ::CModelPartSequenceDefHeader;
using ::CModelPartFrameWindowRecord;
using ::CModelPartSequenceFrameRecord;
using ::CModelPartStateDefRecord;
using ::CModelPartWeightVisibilityRecord;
using ::CModelVisibilityDependencyRecord;
using ::CModelPartStateController;
using ::CGxuLight;
using ::RenderOverrideLocalPointOutputRecord;
using ::ParticleEmitterOverrideSlot;
using ::PlaneParticleEmitterOverrideSlot;
using ::CAnimRibbonObjStatus;
using ::AnimRibbonOverrideSlot;
using ::CParticleEmitterRuntimeParticle;
using ::CParticleEmitterRuntime;
using ::CPlaneParticleEmitter;
using ::CAnimRibbonObj;
using ::SceneNodeChildLink;
using ::SceneNodeChildBucket;
using ::RenderablePart;
using ::RenderBatchElement;
using ::MeshLayerStateRecord;
using ::MeshLayerDispatchRecord;
using ::MeshAuxResourceEntry;
using ::LayerInfo;
using ::MeshInfo;
using ::MeshData;
} // namespace analysis

// 渲染批次元素（24 字节），ExecBatch 的 element
struct WorldObjectBatchEntry {
    void* vtable;        // 0x00
    uint32_t flags;      // 0x04
    uint32_t unknown_id; // 0x08：视角内递增，触顶后归零（疑似队列序号）
    void* geoset_data;   // 0x0C
    uint32_t reserved;   // 0x10
    void* scene_node;    // 0x14：与 WorldObjectEntry + 0x20 对齐的 key
};

// WorldObjects 列表元素（每项 24 字节）
struct WorldObjectListEntry {
    void* world_object_entry; // 0x00
    uint32_t unk_0x04;        // 0x04
    uint32_t unk_0x08;        // 0x08
    uint32_t unk_0x0C;        // 0x0C
    uint32_t unk_0x10;        // 0x10
    void* unit_ptr;           // 0x14：实测是 CUnit*
};

// WorldObjectEntry：仅记录已确认字段
struct WorldObjectEntry {
    void* vtable; // 0x00
    uint8_t pad_0x04[0x1C];
    void* scene_node; // 0x20：与 WorldObjectBatchEntry + 0x14 对齐
};

// CAgentBaseAbs（简化，仅保留类型与 Unit 指针）
struct CAgentBaseAbs {
    void* vtable;        // 0x00
    uint8_t pad_0x04[0x08];
    HashGroup type_hash; // 0x0C：通常可读为 +w3u/+w3d 等类型签名
    uint8_t pad_0x14[0x40];
    void* unit_ptr;      // 0x54：指向 CUnit
};


// 游戏哈希表桶
struct GameHashTableBucket {
    uint32_t step;    // 0x00
    uintptr_t tail;   // 0x04
    uintptr_t entry;  // 0x08
};

// 游戏哈希表
struct GameHashTable {
    void** vtable;                // 0x00
    uint32_t off;                 // 0x04
    uintptr_t tail;               // 0x08
    intptr_t head;                // 0x0C
    uint8_t pad_0x10[0x0C];       // 0x10
    GameHashTableBucket* buckets; // 0x1C
    uint8_t pad_0x20[0x04];       // 0x20
    uint32_t mask;                // 0x24
    uint32_t size;                // 0x28
};


namespace CUnitOffsets {
    constexpr size_t VTable       = 0x00;
    constexpr size_t RefCount     = 0x04;
    constexpr size_t HashId0C     = 0x0C;  // 可能的 handleId
    constexpr size_t HashId10     = 0x10;  // 备用 hashId
    constexpr size_t Sprite       = 0x28;  // CSprite*
    constexpr size_t Rawcode      = 0x30;  // 四字码 (如 'hfoo')
    constexpr size_t RuntimeNode48 = 0x48; // 2026-07-08 实机：非 buildingShadow 字符串
    constexpr size_t RuntimeNode50 = 0x50; // 2026-07-08 实机：运行期 node/list，非 buildingShadow
    constexpr size_t OwnerId      = 0x58;  // 所有者玩家ID
    constexpr size_t Flags5C      = 0x5C;  // 标志位
    constexpr size_t Flags60      = 0x60;  // 更多标志
    constexpr size_t FlyHeight    = 0x208; // 当前飞行高度

    constexpr uint32_t FlagBuilding = 0x00010000u;
}

// CUnitUIManager 每个 unit type 的 UI/type record。该记录不是 CUnit 实例本体：
// CUnit+0x50 实机确认是运行期 node/list；UnitUI.slk 的 unitShadow/buildingShadow
// 字符串写在本 record 的 +0x4C/+0x50。
namespace CUnitUiTypeRecordOffsets {
    constexpr size_t VTable                = 0x00;
    constexpr size_t RawcodeFourCC         = 0x18; // 数值 rawcode；内存字节为 little-endian 反序
    constexpr size_t RawcodeAscii          = 0x1C; // inline ASCII，如 "halt"/"hctw"
    constexpr size_t ModelPath0            = 0x34; // char*
    constexpr size_t ModelPath1            = 0x38; // char*
    constexpr size_t UberSplatKey          = 0x48; // char*，如 "HMED"/"HSMA"
    constexpr size_t UnitShadowTexture     = 0x4C; // char*，UnitUI.slk unitShadow
    constexpr size_t BuildingShadowTexture = 0x50; // char*，UnitUI.slk buildingShadow
    constexpr size_t ShadowOffsetX         = 0x80;
    constexpr size_t ShadowOffsetY         = 0x84;
    constexpr size_t ShadowSizeX           = 0x88;
    constexpr size_t ShadowSizeY           = 0x8C;
    constexpr size_t ShadowOnWater         = 0x90;
}

// Analysis-only 32-bit layout for CUnitUiTypeRecord. Pointer-valued fields are
// stored as uint32_t so the documented offsets remain stable in host tools.
struct CUnitUiTypeRecord32 {
    uint32_t vtable;                  // 0x00
    uint8_t pad_0x04[0x14];           // 0x04
    uint32_t rawcodeFourCC;           // 0x18
    uint32_t rawcodeAscii;            // 0x1C
    uint8_t pad_0x20[0x14];           // 0x20
    uint32_t modelPath0;              // 0x34
    uint32_t modelPath1;              // 0x38
    uint8_t pad_0x3C[0x0C];           // 0x3C
    uint32_t uberSplatKey;            // 0x48
    uint32_t unitShadowTexture;       // 0x4C
    uint32_t buildingShadowTexture;   // 0x50
    uint8_t pad_0x54[0x2C];           // 0x54
    float shadowOffsetX;              // 0x80
    float shadowOffsetY;              // 0x84
    float shadowSizeX;                // 0x88
    float shadowSizeY;                // 0x8C
    uint32_t shadowOnWater;           // 0x90
};

static_assert(offsetof(CUnitUiTypeRecord32, buildingShadowTexture) ==
                  CUnitUiTypeRecordOffsets::BuildingShadowTexture,
              "CUnitUiTypeRecord32 buildingShadow offset mismatch");
static_assert(offsetof(CUnitUiTypeRecord32, shadowOnWater) ==
                  CUnitUiTypeRecordOffsets::ShadowOnWater,
              "CUnitUiTypeRecord32 shadowOnWater offset mismatch");

namespace CWidgetOffsets {
    constexpr size_t VTable       = 0x00;
    constexpr size_t RefCount     = 0x04;
    constexpr size_t HashId0C     = 0x0C;
    constexpr size_t HashId10     = 0x10;
    constexpr size_t Flags20      = 0x20;
    constexpr size_t Sprite       = 0x28;  // CSprite*
    constexpr size_t TypeId       = 0x30;  // rawcode / type id
}

namespace CSpriteOffsets {
    constexpr size_t VTable             = 0x00;
    constexpr size_t RefCount           = 0x04;
    constexpr size_t Model              = 0x20;
    constexpr size_t ParentSprite       = 0x24;
    constexpr size_t SpriteFlags        = 0x28;
    constexpr size_t CachedSequenceId   = 0x2C;
    constexpr size_t AttachPointIndex   = 0x2E;
    constexpr size_t SequenceProvider   = 0x30;
    constexpr size_t AnimQueueCount     = 0x34;
    constexpr size_t AnimQueueHead      = 0x35;
    constexpr size_t AnimQueueTail      = 0x36;
    constexpr size_t AnimQueueStorageHint = 0x38;
    constexpr size_t AnimQueueCapacity  = 0x3C;
    constexpr size_t AnimQueueEntries   = 0x40;  // CSpriteAnimRequest*
}

namespace CSpriteUberOffsets {
    constexpr size_t AnimationTimeOverrideEnabled = 0x94;
    constexpr size_t AnimationTimeOverrideValue   = 0xA0;
    constexpr size_t WorldX                       = 0xC0;
    constexpr size_t WorldY                       = 0xC4;
    constexpr size_t WorldZ                       = 0xC8;
    constexpr size_t UniformScale                 = 0xE8;
    constexpr size_t WorldMatrix3x4               = 0x108; // float[12]
}

namespace CModelAnimControllerOffsets {
    constexpr size_t VTable        = 0x00;
    constexpr size_t AnimComplex   = 0x98;
}

namespace CAnimComplexOffsets {
    constexpr size_t TimeStates            = 0x08;
    constexpr size_t SequenceCount         = 0x0C;
    constexpr size_t OnSequenceEndCallback = 0x3C;
    constexpr size_t OnSequenceEndContext  = 0x40;
    constexpr size_t SequenceProvider      = 0x44;
    constexpr size_t TimeScale             = 0x48;
    constexpr size_t Flags                 = 0x54;
    constexpr size_t CurrentSequenceIndex  = 0x58;
    constexpr size_t BoneBlendBuffer       = 0x70;
    constexpr size_t BoneBlendCount        = 0x74;
    constexpr size_t BlendTransitionTime   = 0xE4;
}

namespace CAnimSequenceProviderOffsets {
    constexpr size_t ExternalToInternalMap = 0x08;
    constexpr size_t ExternalSequenceCount = 0x0C;
    constexpr size_t SequenceData          = 0x18;
    constexpr size_t SequenceCount         = 0x1C;
}

namespace CSpriteFlagBits {
    constexpr uint32_t AttachedToParent = 0x10000;
    constexpr uint32_t AttachmentRefresh = 0x200000;
    constexpr uint32_t AttachmentEnabled = 0x400000;
}

namespace CEffectOffsets {
    constexpr size_t AttachFlags       = 0x20;
    constexpr size_t Sprite            = 0x28;  // CSprite*
    constexpr size_t AttachPointCount  = 0x4C;
    constexpr size_t AttachPointIds    = 0x50;
    constexpr size_t BoundAgentHash    = 0x78;  // HashGroup
}

namespace SceneNodeOffsets {
    constexpr size_t RenderableCount        = 0x0C;
    constexpr size_t RenderableList         = 0x10;
    constexpr size_t CullTable              = 0x20;  // SceneNodeTintRecord*
    constexpr size_t MeshInfoTable          = 0x30;  // MeshInfo*[]
    constexpr size_t VisibilityTable        = 0x50;  // uint8_t*
    constexpr size_t WorldMatrix            = 0x64;  // float[12]
    constexpr size_t Flags                  = 0x94;
    constexpr size_t ChildVisibilityCtx     = 0x98;
    constexpr size_t World                  = 0x9C;
    constexpr size_t StagePresetBaseIndex   = 0xA0;
    constexpr size_t ChildCount             = 0xC4;
    constexpr size_t ChildBuckets           = 0xC8;  // SceneNodeChildBucket*
    constexpr size_t ChildVisibilityCache   = 0xD4;  // uint8_t*
}

namespace RenderablePartFieldOffsets {
    constexpr size_t StagePresetSpanBaseIndex = 0x08;
    constexpr size_t MeshData                 = 0x0C;
    constexpr size_t SkipFlag                 = 0x10;
    constexpr size_t SceneNode                = 0x14;
}

namespace SceneNodeChildLinkOffsets {
    constexpr size_t NextLink   = 0x04;
    constexpr size_t ChildScene = 0x08;
    constexpr size_t LinkFlags  = 0x0C;
}

namespace SceneNodeFlagBits {
    constexpr uint32_t HasTransparentSubLists = 0x10;
}

namespace RenderBatchElementOffsets {
    constexpr size_t BatchEntry   = 0x00;  // RenderablePart*
    constexpr size_t Flags        = 0x04;
    constexpr size_t LayerIndex   = 0x08;
    constexpr size_t LayerCounter = 0x0C;
    constexpr size_t LayerState   = 0x10;  // MeshLayerStateRecord*
}

namespace RenderBatchElementFlagBits {
    constexpr uint32_t MeshFlag            = 0x01;
    constexpr uint32_t HasMoreVisibleLayer = 0x02;
}

namespace MeshDataOffsets {
    constexpr size_t PrimaryStreamArg0     = 0x0C;
    constexpr size_t PrimaryStreamPtr      = 0x10;
    constexpr size_t PrimaryStreamStride   = 0x48;
    constexpr size_t Stream1Ptr            = 0x4C;
    constexpr size_t Stream1Stride         = 0x58;
    constexpr size_t AuxLayerResourceTable = 0x94;
    constexpr size_t SubPrimitiveCount     = 0xC8;
    constexpr size_t SubPrimitivePairs     = 0xCC;
    constexpr size_t PrimitiveBaseIndex    = 0xE0;
    constexpr size_t TransformOrPoseCtx    = 0xF0;
    constexpr size_t MeshFlag              = 0x104;
    constexpr size_t MeshIndex             = 0x108;
    constexpr size_t BoundingPos           = 0x10C;  // float[3]
    constexpr size_t CullIndex             = 0x11C;
    constexpr size_t TransparentKey        = 0x120;
    constexpr size_t ExtraMeshFlags        = 0x124;
}

namespace MeshInfoOffsets {
    constexpr size_t LayerCount   = 0x0C;
    constexpr size_t LayerStates  = 0x10;  // MeshLayerStateRecord*
    constexpr size_t LayerInfo    = 0x38;  // LayerInfo*
}

namespace LayerInfoOffsets {
    constexpr size_t LayerRecords = 0x10;  // MeshLayerDispatchRecord*
}

namespace MeshLayerDispatchRecordOffsets {
    constexpr size_t StagePresetIndex0  = 0x0C;
    constexpr size_t StagePresetIndex1  = 0x10;
    constexpr size_t AuxRefIndex0     = 0x14;
    constexpr size_t AuxRefIndex1     = 0x18;
    constexpr size_t VisibilityOffset = 0x1C;
    constexpr size_t AlphaFlags       = 0x20;
    constexpr size_t StageMode0       = 0x24;
    constexpr size_t StageMode1       = 0x28;
}

namespace MeshLayerDispatchRecordFlagBits {
    constexpr uint32_t DualAlphaSubmit = 0x01;
}

namespace MeshLayerStateRecordOffsets {
    constexpr size_t PrimaryResourceBinding = 0x00;
    constexpr size_t BlendOrDrawMode  = 0x18;
    constexpr size_t AuxRefEnable0    = 0x1C;
    constexpr size_t AuxRefEnable1    = 0x20;
}

namespace MeshAuxResourceEntryOffsets {
    constexpr size_t ResourceBinding  = 0x08;
}

namespace CModelArrayHeaderOffsets {
    constexpr size_t Capacity        = 0x00;
    constexpr size_t Count           = 0x04;
    constexpr size_t Data            = 0x08;
    constexpr size_t GrowthAlignment = 0x0C;
}

namespace CGeosetUvLayerRecordOffsets {
    constexpr size_t Capacity = 0x00;
    constexpr size_t Count    = 0x04;
    constexpr size_t Data     = 0x08;
    constexpr size_t InlineUv = 0x0C;
}

namespace CGeosetPrimitiveRecordOffsets {
    constexpr size_t PrimitiveTypeOrMaterialSlot = 0x00;
    constexpr size_t IndexCount                  = 0x04;
}

namespace CModelGeosetBindingRecordOffsets {
    constexpr size_t LayoutOrSlotA = 0x00;
    constexpr size_t LayoutOrSlotB = 0x04;
    constexpr size_t ScaleOrWeightA = 0x08;
    constexpr size_t ScaleOrWeightB = 0x0C;
}

namespace CGeosetOffsets {
    constexpr size_t GeosetData           = 0x0C;
    constexpr size_t MaterialOrLayoutSlot = 0x10;
}

namespace CGeosetDataOffsets {
    constexpr size_t VertexCapacity           = 0x08;
    constexpr size_t VertexCount              = 0x0C;
    constexpr size_t VertexPositions          = 0x10;
    constexpr size_t VertexGroupCapacity      = 0x44;
    constexpr size_t VertexGroupCount         = 0x48;
    constexpr size_t VertexGroupIndices       = 0x4C;
    constexpr size_t NormalCapacity           = 0x50;
    constexpr size_t NormalCount              = 0x54;
    constexpr size_t NormalVectors            = 0x58;
    constexpr size_t UvLayerCapacity          = 0x8C;
    constexpr size_t UvLayerCount             = 0x90;
    constexpr size_t UvLayers                 = 0x94;
    constexpr size_t PrimitiveRecordCapacity  = 0xC4;
    constexpr size_t PrimitiveRecordCount     = 0xC8;
    constexpr size_t PrimitiveRecords         = 0xCC;
    constexpr size_t IndexCapacity            = 0xD8;
    constexpr size_t IndexCount               = 0xDC;
    constexpr size_t IndexBufferU16           = 0xE0;
    constexpr size_t MatrixGroupSizeCapacity  = 0xEC;
    constexpr size_t MatrixGroupCount         = 0xF0;
    constexpr size_t MatrixGroupSizes         = 0xF4;
    constexpr size_t MatrixIndexCapacity      = 0xF8;
    constexpr size_t MatrixIndexCount         = 0xFC;
    constexpr size_t MatrixIndices            = 0x100;
    constexpr size_t LayoutOrMaterialMeta0    = 0x104;
    constexpr size_t LayoutOrMaterialSlot     = 0x108;
    constexpr size_t MergedGeosetBindingIndex = 0x11C;
}

namespace CModelDataOffsets {
    constexpr size_t GeosetCapacity            = 0x08;
    constexpr size_t GeosetCount               = 0x0C;
    constexpr size_t Geosets                   = 0x10;
    constexpr size_t GeosetBindingCapacity     = 0x18;
    constexpr size_t GeosetBindingCount        = 0x1C;
    constexpr size_t GeosetBindings            = 0x20;
    constexpr size_t MaterialCapacity          = 0x28;
    constexpr size_t MaterialCount             = 0x2C;
    constexpr size_t MaterialHandles           = 0x30;
    constexpr size_t MaterialSlotCapacity      = 0x48;
    constexpr size_t MaterialSlotCount         = 0x4C;
    constexpr size_t MaterialSlotBytes         = 0x50;
    constexpr size_t Flags                     = 0x94;
    constexpr size_t SharedResourceProto       = 0x98;
    constexpr size_t ModelDataHandle           = 0x9C;
    constexpr size_t LightTemplateCount        = 0xB8;
    constexpr size_t LightTemplates            = 0xBC;
    constexpr size_t ChildRuntimeGroupRecords  = 0xC8;
    constexpr size_t ChildRuntimeGroupCount    = 0xD0;
    constexpr size_t PlaneParticleEmitterCount = 0xE8;
    constexpr size_t PlaneParticleEmitters     = 0xEC;
    constexpr size_t CameraCount               = 0x100;
    constexpr size_t Cameras                   = 0x104;
}

namespace CModelOffsets {
    constexpr size_t RuntimeGeosetCapacity = 0x08;
    constexpr size_t RuntimeGeosetCount    = 0x0C;
    constexpr size_t RuntimeGeosets        = 0x10;
    constexpr size_t GeosetBindingCapacity = 0x18;
    constexpr size_t GeosetBindingCount    = 0x1C;
    constexpr size_t GeosetBindingRecords  = 0x20;
    constexpr size_t RuntimeMaterialCapacity = 0x28;
    constexpr size_t RuntimeMaterialCount    = 0x2C;
    constexpr size_t RuntimeMaterials        = 0x30;
    constexpr size_t RuntimeExtraHandleCapacity = 0x48;
    constexpr size_t RuntimeExtraHandleCount    = 0x4C;
    constexpr size_t RuntimeExtraHandles        = 0x50;
    constexpr size_t FinalPoseMatrixCount  = 0x5C;
    constexpr size_t FinalPoseMatrixArray  = 0x60;
    constexpr size_t WorldMatrix3x4        = 0x64;
    constexpr size_t Flags                 = 0x94;
    constexpr size_t PartStateController   = 0x98;
    constexpr size_t OwnedModelDataHandle  = 0x9C;
    constexpr size_t DeferredCallbackCount = 0xA8;
    constexpr size_t DeferredCallbackArray = 0xAC;  // stride 0x24
    constexpr size_t ChildBucketCount      = 0xC4;
    constexpr size_t ChildBucketArray      = 0xC8;  // SceneNodeChildBucket* 同布局复用
    constexpr size_t ChildStateCache       = 0xD4;  // uint8_t*，缓存 sub_6F777FE0 结果
}

namespace CModelFlagBits {
    constexpr uint32_t HasNonUnitScale = 0x04;
    constexpr uint32_t HasComplexTree  = 0x10;
}

namespace RenderStagePresetOverrideNodeOffsets {
    constexpr size_t SourceRecordIndex   = 0xA8;
    constexpr size_t TransformIndex      = SourceRecordIndex; // 兼容旧命名
    constexpr size_t OutputSlotIndex     = 0xAC;
    constexpr size_t NodeType            = 0xB0;
    constexpr size_t ModeBits            = 0xB1;
    constexpr size_t ChildCount          = 0xB8;
    constexpr size_t ChildArray          = 0xBC;
    constexpr size_t VisibilityMaskIndex = 0xC4;
}

namespace RenderOverrideGraphOutputBundleOffsets {
    constexpr size_t PrimaryPresetOutputs             = 0x08;
    constexpr size_t GxuLightArrayHandle              = 0x14;
    constexpr size_t ParticleEmitterRuntimeArrayHandle= 0x18;
    constexpr size_t SharedPresetOutputs              = 0x24;
    constexpr size_t DependencyGateOutputs            = 0x2C;
    constexpr size_t VisibilityByteOutputsHandle      = 0x38;
    constexpr size_t CompactScalarOutputs             = 0x3C;
}

namespace CModelPartSequenceDefHeaderOffsets {
    constexpr size_t SequenceFrameRecords = 0x18;
    constexpr size_t WrappedFramePeriods  = 0x20;
}

namespace CModelPartFrameWindowRecordOffsets {
    constexpr size_t CurrentFrameIndex       = 0x00;
    constexpr size_t FrameStateWithLoopBit   = 0x04;
    constexpr size_t OnPartLoopOrComplete    = 0x08;
    constexpr size_t OnPartLoopOrCompleteCtx = 0x0C;
}

namespace CModelPartSequenceFrameRecordOffsets {
    constexpr size_t FrameStart = 0x00;
    constexpr size_t FrameEnd   = 0x04;
    constexpr size_t Flags      = 0x0C;
}

namespace CModelPartStateDefRecordOffsets {
    constexpr size_t VisibilityDependencyIndex = 0xE0;
}

namespace CModelPartWeightVisibilityRecordOffsets {
    constexpr size_t ResolvedWeight = 0x34;
}

namespace CModelVisibilityDependencyRecordOffsets {
    constexpr size_t Flags = 0x18;
}

namespace CModelPartStateControllerOffsets {
    constexpr size_t FrameWindowArray         = 0x08;
    constexpr size_t GlobalLoopCallback       = 0x3C;
    constexpr size_t GlobalLoopCallbackCtx    = 0x40;
    constexpr size_t SequenceDefHeader        = 0x44;
    constexpr size_t FrameTimeScale           = 0x48;
    constexpr size_t LastTickMs               = 0x4C;
    constexpr size_t FrameDelta               = 0x50;
    constexpr size_t Flags                    = 0x54;
    constexpr size_t CurrentPartIndex         = 0x58;
    constexpr size_t WrappedFrameOffsets      = 0x5C;
    constexpr size_t WrappedFrameOffsetCount  = 0x60;
    constexpr size_t ParticleEmitterSlots     = 0x94;
    constexpr size_t ParticleEmitterSlotCount = 0x98;
    constexpr size_t PlaneParticleSlots       = 0x9C;
    constexpr size_t PlaneParticleSlotCount   = 0xA0;
    constexpr size_t AnimRibbonStatuses       = 0xA4;
    constexpr size_t AnimRibbonStatusCount    = 0xA8;
    constexpr size_t AnimRibbonSlots          = AnimRibbonStatuses;
    constexpr size_t AnimRibbonSlotCount      = AnimRibbonStatusCount;
}

namespace CGxuLightOffsets {
    constexpr size_t StateFlag              = 0x00;
    constexpr size_t UseInputPointTransform = 0x04;
    constexpr size_t PositionOrDirection    = 0x08; // float[3]
    constexpr size_t AmbientColorPacked     = 0x14;
    constexpr size_t DirectionalColorPacked = 0x18;
    constexpr size_t AmbientIntensity       = 0x1C;
    constexpr size_t DirectionalIntensity   = 0x20;
    constexpr size_t RefCount               = 0x24;
    constexpr size_t MaxDistanceOrRange     = 0x28;
}

namespace RenderOverrideLocalPointOutputRecordOffsets {
    constexpr size_t ResolvedLocalPoint = 0x34; // float[3]
}

namespace ParticleEmitterOverrideSlotOffsets {
    constexpr size_t GateResolveState = 0x70;
    constexpr size_t EmissionScale    = 0x7C;
}

namespace PlaneParticleEmitterOverrideSlotOffsets {
    constexpr size_t GateResolveState     = 0x58;
    constexpr size_t PlaneParticleEmitter = 0x88;
}

namespace CAnimRibbonObjStatusOffsets {
    constexpr size_t GateResolveState = 0x28;
    constexpr size_t AnimRibbonObj     = 0x70;
}

namespace AnimRibbonOverrideSlotOffsets {
    constexpr size_t GateResolveState = CAnimRibbonObjStatusOffsets::GateResolveState;
    constexpr size_t AnimRibbonRuntime = CAnimRibbonObjStatusOffsets::AnimRibbonObj;
}

namespace CParticleEmitterRuntimeParticleOffsets {
    constexpr size_t RemainingLife         = 0x00;
    constexpr size_t SpawnPhaseOffset      = 0x04;
    constexpr size_t Position              = 0x08; // float[3]
    constexpr size_t Velocity              = 0x14; // float[3]
    constexpr size_t RenderScaleOrSize     = 0x20;
    constexpr size_t ModelInstanceOrSprite = 0x24;
}

namespace CParticleEmitterRuntimeOffsets {
    constexpr size_t SpawnAccumulator  = 0x04;
    constexpr size_t EmissionRateScale = 0x10;
    constexpr size_t ParticleArray     = 0x38; // CParticleEmitterRuntimeParticle*
    constexpr size_t ActiveIndices     = 0x48;
    constexpr size_t ActiveCount       = 0x50;
    constexpr size_t FreeIndices       = 0x5C;
    constexpr size_t FreeCount         = 0x64;
}

namespace CPlaneParticleEmitterOffsets {
    constexpr size_t Flags          = 0x194;
    constexpr size_t WorldMatrix3x4 = 0x198; // float[12]
}

namespace CAnimRibbonObjOffsets {
    constexpr size_t LastTickMs         = 0x18;
    constexpr size_t PhaseAccumulator   = 0x1C;
    constexpr size_t HasCachedTransform = 0x20;
    constexpr size_t Flags              = 0x150;
    constexpr size_t Ukn164             = 0x164;
    constexpr size_t Ukn168             = 0x168;
}

// CUnit flags at offset 0x5C
namespace UnitFlags5C {
    constexpr uint32_t Loaded        = 0x10;
    constexpr uint32_t Dead          = 0x100;
    constexpr uint32_t Decay         = 0x800;
    constexpr uint32_t TimedLife     = 0x1000;
    constexpr uint32_t Building      = 0x10000;  // 关键：建筑标志
    constexpr uint32_t MinimapGold   = 0x20000;
    constexpr uint32_t MinimapTavern = 0x40000;
    constexpr uint32_t MinimapHide   = 0x80000;
    constexpr uint32_t Stun          = 0x100000;
    constexpr uint32_t Pause         = 0x200000;
    constexpr uint32_t Invisible     = 0x1000000;
    constexpr uint32_t OnBuilding    = 0x2000000;
    constexpr uint32_t Illusion      = 0x40000000;
}

// CUnit flags at offset 0x60
namespace UnitFlags60 {
    constexpr uint32_t Critter      = 0x1;   // 小动物
    constexpr uint32_t Tornado      = 0x2;   // 龙卷风
    constexpr uint32_t Selectable   = 0x20;  // 可选择
}

// ============================================================================
// CAgentBaseAbs 偏移
// ============================================================================

namespace CAgentOffsets {
    constexpr size_t VTable     = 0x00;
    constexpr size_t TypeHashGroup = 0x0C;  // HashGroup 结构起始
    constexpr size_t TypeHashId    = 0x0C;  // HashGroup.hash_0x0 - 类型 ID
    constexpr size_t TypeFourCC    = 0x10;  // HashGroup.hash_0x4 - 类型 FourCC ('+w3u' 等)
    constexpr size_t UnitPtr       = 0x54;  // CUnit* 指针
}

// 常见类型标识 (从 Agent 的 HashGroup 读取)
// 
// 注意：不同类型的对象，类型标识存放位置不同：
// - 单位 (Unit): 类型值通常在 offset 0x10，值为 '+w3u' (0x2B773375)
// - 可破坏物 (Destructible): 类型值通常在 offset 0x0C，值为类型 ID (如 0x029B)
// - 物品 (Item): 类型值通常在 offset 0x10，值为 '+w3i' (0x2B773369)
//
// 因此 AgentWrapper::GetTypeFourCC() 会优先验证 0x10 是否为 +w3u/+w3d/+w3i，
// 仅在 0x0C 命中已知类型 ID（如可破坏物）时才返回 0x0C
namespace AgentTypeFourCC {
    // FourCC 常量 (从 offset 0x10 读取，适用于单位/物品)
    constexpr uint32_t Unit        = 0x75337727;  // '+w3u' 按 MakeFourCC 计算
    constexpr uint32_t Destructible= 0x64337727;  // '+w3d' (不常用)
    constexpr uint32_t Item        = 0x69337727;  // '+w3i'
    
    // 备用：直接匹配内存中读取的值 (小端字节序，从 offset 0x10)
    constexpr uint32_t Unit_LE        = 0x2B773375;  // 内存中直接读取
    constexpr uint32_t Destructible_LE= 0x2B773364;  // 内存中直接读取 (不常用)
    constexpr uint32_t Item_LE        = 0x2B773369;  // 内存中直接读取
    
    // 类型 ID 常量 (从 offset 0x0C 读取，适用于可破坏物等)
    // 这些值是通过实际观察得出的，可能因版本而异
    constexpr uint32_t DestructibleID = 0x0000029B;  // 可破坏物常见类型 ID
    // 可根据需要添加更多类型 ID
}

// JassHandleTable 偏移（与 d3d9_war3_structs.h 中的 JassHandleTable 结构对应）
// struct JassHandleTable {
//   uint32_t capacity;          // 0x00
//   uint32_t size;              // 0x04  
//   JassHandleNode* handle_array; // 0x08
//   uint32_t pad;               // 0x0C
// };
namespace HandleTableOffsets {
    constexpr size_t Capacity    = 0x00;
    constexpr size_t Size        = 0x04;
    constexpr size_t HandleArray = 0x08;  // 注意：是 0x08 不是 0x0C
}

// ============================================================================
// Handle 计算便捷函数
// ============================================================================

inline uint32_t MakeJHandle(uint32_t handleId) {
    return handleId | 0x100000;
}

inline uint32_t GetHandleId(uint32_t jHandle) {
    return jHandle & 0x0FFFFF;
}

inline bool IsValidHandle(uint32_t jHandle) {
    return (jHandle & 0x100000) != 0 && (jHandle & 0x0FFFFF) != 0;
}

// ============================================================================
// WorldObject 渲染相关偏移
// ============================================================================

// WorldObjectBatchEntry 结构 (ExecBatch element，24 字节)
namespace ElementOffsets {
    constexpr size_t VTable     = 0x00;
    constexpr size_t Flags      = 0x04;
    constexpr size_t UnknownId  = 0x08;
    constexpr size_t GeosetData = 0x0C;
    constexpr size_t Reserved   = 0x10;
    constexpr size_t SceneNode  = 0x14;  // SceneNode* (关键！)
}

// WorldObjects 列表元素结构 (24 字节)
namespace ListElementOffsets {
    constexpr size_t WorldObjectEntry = 0x00;  // WorldObjectEntry*
    constexpr size_t UnitPtr          = 0x14;  // CUnit* (关键！)
}
constexpr size_t ListElementSize = 24;

// WorldObjectEntry 偏移
namespace WorldObjectEntryOffsets {
    constexpr size_t VTable    = 0x00;
    constexpr size_t SceneNode = 0x20;  // SceneNode*
}

// ============================================================================
// 游戏全局变量偏移 (1.27a)
// ============================================================================

namespace GameDllOffsets {
    constexpr uintptr_t GameWar3Ptr  = 0xBE4238;  // CGameWar3*
    constexpr uintptr_t GameStateOff = 0x1C;       // CGameWar3 -> CGameState
    constexpr uintptr_t HandleTableOff = 0x194;    // CGameState -> JassHandleTable
}

// ============================================================================
// FourCC 工具函数
// ============================================================================

constexpr inline uint32_t MakeFourCC(char a, char b, char c, char d) {
    return static_cast<uint32_t>(static_cast<uint8_t>(a)) |
           (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24);
}

inline void FourCCToString(uint32_t value, char out[5]) {
    out[0] = static_cast<char>(value & 0xFF);
    out[1] = static_cast<char>((value >> 8) & 0xFF);
    out[2] = static_cast<char>((value >> 16) & 0xFF);
    out[3] = static_cast<char>((value >> 24) & 0xFF);
    out[4] = '\0';
}

inline bool IsLikelyFourCC(uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        const uint8_t ch = static_cast<uint8_t>((value >> (i * 8)) & 0xFFu);
        if (ch < 0x20 || ch > 0x7E)
            return false;
    }
    return true;
}

} // namespace dxvk::war3
