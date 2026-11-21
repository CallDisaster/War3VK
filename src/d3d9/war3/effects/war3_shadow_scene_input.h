#pragma once

// From the shadow records of a frame (war3_shadow_scene.h) to the input of
// the shadow pass.
//
// War3ShadowReceiverPass::Run takes a War3PipelineInput: targets, settings,
// the world camera and the frame's casters. In the game the x86 device fills
// it from its own capture. An executor that only has records - the x64 host,
// and the in-process arm of the two-mode fixture - fills it with the
// functions below, so both build the same input from the same records and
// differ only in where the caster's buffers come from:
//   x64 host:    the host's copy of the draw's vertex and index buffer;
//   in process:  the D3D9 buffer's own DxvkBuffer;
// and a dynamic buffer, which the caller may rewrite before the pass replays
// it, is copied at the draw into the executor's own pool (ShadowCasterPool)
// from the bytes the draw read.
//
// This is the executor side of the records. The producer side - the x86
// capture emitting ShadowCaster records for the game's draws - is not here.

#include "war3_shadow_scene.h"

#include "../../d3d9_war3_light.h"
#include "../../d3d9_war3_shadow.h"
#include "../../d3d9_war3_volumetric_light.h"
#include "../core/war3_runtime_profile.h"
#include "../render/war3_render_state.h"
#include "../render/war3_shadow_lifecycle.h"
#include "../render/war3_weather_runtime.h"
#include "../state/war3_fog_volume.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

namespace dxvk::war3::effects {

// ---- Lighting <-> War3RenderSettings ---------------------------------------

// The Lighting table names every field of these structs. A field added to one
// of them changes its size: add the field to WAR3_SCENE_LIGHTING_FIELDS, then
// update the size here.
static_assert(sizeof(War3DirectionalLightSettings) == 40 && sizeof(War3ShadowSettings) == 280
    && sizeof(War3DayNightSettings) == 104 && sizeof(War3RenderSettings::ambient) == 20,
    "a lighting setting was added or removed: carry it in WAR3_SCENE_LIGHTING_FIELDS");
static_assert(scene::LightingBytes == 492, "Lighting record layout");
// The Outline record names every field of War3OccludedOutlineSettings.
static_assert(sizeof(War3OccludedOutlineSettings) == 28,
    "an outline setting was added or removed: carry it in the Outline record");
// The VolumetricLight table names every field of War3VolumetricLightSettings.
static_assert(sizeof(War3VolumetricLightSettings) == 148,
    "a volumetric light setting was added or removed: carry it in WAR3_SCENE_VOLUMETRIC_FIELDS");
static_assert(scene::VolumetricLightBytes == 164, "VolumetricLight record layout");

inline scene::Lighting LightingFromSettings(const War3RenderSettings& s) {
  scene::Lighting l;
#define WAR3_SL_BOOL(name, path, ...) l.name = s.path ? 1u : 0u;
#define WAR3_SL_VALUE(name, path, ...) l.name = s.path;
#define WAR3_SL_ENUM(name, path, ...) l.name = uint32_t(s.path);
#define WAR3_SL_VECTOR(name, path, ...) l.name = { s.path.x, s.path.y, s.path.z, s.path.w };
  WAR3_SCENE_LIGHTING_FIELDS(WAR3_SL_BOOL, WAR3_SL_VALUE, WAR3_SL_VALUE, WAR3_SL_VALUE, WAR3_SL_ENUM, WAR3_SL_VECTOR)
#undef WAR3_SL_BOOL
#undef WAR3_SL_VALUE
#undef WAR3_SL_ENUM
#undef WAR3_SL_VECTOR
  return l;
}

inline void ApplyLighting(const scene::Lighting& l, War3RenderSettings& s) {
#define WAR3_SL_BOOL(name, path, ...) s.path = l.name != 0u;
#define WAR3_SL_VALUE(name, path, ...) s.path = l.name;
#define WAR3_SL_ENUM(name, path, ...) s.path = static_cast<decltype(s.path)>(l.name);
#define WAR3_SL_VECTOR(name, path, ...) s.path = Vector4(l.name[0], l.name[1], l.name[2], l.name[3]);
  WAR3_SCENE_LIGHTING_FIELDS(WAR3_SL_BOOL, WAR3_SL_VALUE, WAR3_SL_VALUE, WAR3_SL_VALUE, WAR3_SL_ENUM, WAR3_SL_VECTOR)
#undef WAR3_SL_BOOL
#undef WAR3_SL_VALUE
#undef WAR3_SL_ENUM
#undef WAR3_SL_VECTOR
}

// The table's defaults are the settings' defaults.
inline bool LightingDefaultsAgree() {
  return scene::EncodeLighting(LightingFromSettings(War3RenderSettings())) ==
         scene::EncodeLighting(scene::Lighting());
}

inline scene::VolumetricLight VolumetricFromSettings(const War3VolumetricLightSettings& s) {
  scene::VolumetricLight v;
#define WAR3_SV_BOOL(name, path, ...) v.name = s.path ? 1u : 0u;
#define WAR3_SV_VALUE(name, path, ...) v.name = s.path;
#define WAR3_SV_ENUM(name, path, ...) v.name = uint32_t(s.path);
  WAR3_SCENE_VOLUMETRIC_FIELDS(WAR3_SV_BOOL, WAR3_SV_VALUE, WAR3_SV_VALUE, WAR3_SV_VALUE, WAR3_SV_ENUM)
#undef WAR3_SV_BOOL
#undef WAR3_SV_VALUE
#undef WAR3_SV_ENUM
  return v;
}

inline void ApplyVolumetric(const scene::VolumetricLight& v, War3VolumetricLightSettings& s) {
#define WAR3_SV_BOOL(name, path, ...) s.path = v.name != 0u;
#define WAR3_SV_VALUE(name, path, ...) s.path = v.name;
#define WAR3_SV_ENUM(name, path, ...) s.path = static_cast<decltype(s.path)>(v.name);
  WAR3_SCENE_VOLUMETRIC_FIELDS(WAR3_SV_BOOL, WAR3_SV_VALUE, WAR3_SV_VALUE, WAR3_SV_VALUE, WAR3_SV_ENUM)
#undef WAR3_SV_BOOL
#undef WAR3_SV_VALUE
#undef WAR3_SV_ENUM
}

// The VolumetricLight table's defaults are the settings' defaults.
inline bool VolumetricDefaultsAgree() {
  return scene::EncodeVolumetricLight(VolumetricFromSettings(War3VolumetricLightSettings())) ==
         scene::EncodeVolumetricLight(scene::VolumetricLight());
}

inline void ApplyOutline(const scene::Outline& o, War3OccludedOutlineSettings& s) {
  s.enabled = o.enabled != 0u;
  s.mode = static_cast<War3OutlineMode>(o.mode);
  s.useScreenSpace = o.useScreenSpace != 0u;
  s.widthPx = o.widthPx;
  s.showVisible = o.showVisible != 0u;
  s.showOccluded = o.showOccluded != 0u;
  s.colorR = o.color[0]; s.colorG = o.color[1]; s.colorB = o.color[2]; s.colorA = o.color[3];
}

// The settings the shadow pass and the volumetric light pass run with. What
// the records do not carry is off: the unit outline is drawn only with the
// frame's Outline record, the volumetric light runs only with its
// VolumetricLight record (and the shadow pass renders the volumetric light's
// sun map only then).
inline std::shared_ptr<const War3RenderSettings> ShadowPassSettings(const scene::Lighting& lighting,
    const scene::Outline* outline = nullptr, const scene::VolumetricLight* volumetric = nullptr,
    const War3CasterClasses& classes = {}, const War3SunStyleSettings& sunStyle = {},
    const War3WaterSettings& water = {}) {
  auto settings = std::make_shared<War3RenderSettings>();
  ApplyLighting(lighting, *settings);
  settings->casterClasses = classes;
  settings->sunStyle = sunStyle;
  settings->water = water;
  settings->postFx.volumetricLight.enabled = false;
  settings->occludedOutline.enabled = false;
  if (outline != nullptr)
    ApplyOutline(*outline, settings->occludedOutline);
  if (volumetric != nullptr)
    ApplyVolumetric(*volumetric, settings->postFx.volumetricLight);
  return settings;
}

// ---- camera ------------------------------------------------------------------

// As D3D9DeviceEx::War3RecordWorldCamera: the cascades need a perspective
// projection, and any other camera is not a world camera.
inline War3WorldCameraState ShadowWorldCamera(const scene::Camera& record, uint32_t frameIndex, uint64_t frameSerial) {
  War3WorldCameraState camera;
  for (uint32_t row = 0; row < 4; ++row) {
    for (uint32_t column = 0; column < 4; ++column) {
      camera.view[row][column] = record.view[row * 4 + column];
      camera.proj[row][column] = record.projection[row * 4 + column];
    }
  }
  camera.viewProj = camera.proj * camera.view;
  camera.invViewProj = inverse(camera.viewProj);
  camera.viewport = { record.x, record.y, record.width, record.height, record.minZ, record.maxZ };
  camera.scissor = { LONG(record.x), LONG(record.y), LONG(record.x + record.width), LONG(record.y + record.height) };
  camera.frameIndex = frameIndex;
  camera.frameSerial = frameSerial;
  camera.valid = std::abs(std::abs(camera.proj[2][3]) - 1.0f) <= 1e-3f && std::abs(camera.proj[3][3]) <= 1e-3f;
  return camera;
}

// ---- the executor's caster pool ------------------------------------------------

// Where an executor keeps what a caster's draw read from a dynamic buffer. The
// D3D9 caller rewrites such a buffer whenever it likes - the next
// Lock(DISCARD), or an ordinary Lock the device must first let the GPU finish
// for - and the pass replays the caster only at BeforeUi, so the bytes are
// copied at the draw, as the in-process capture does into its own arena.
//
// A frame's copies go into host-visible chunks created for that frame. The
// casters hold their chunks, the command list that replays them holds them
// until it retires, and then they go back to the device. Nothing of one frame
// is reused by the next: a map or a device that ended leaves nothing here.
class ShadowCasterPool {
public:
  explicit ShadowCasterPool(const Rc<DxvkDevice>& device) : m_device(device) { }

  struct Slice {
    Rc<DxvkBuffer> buffer;
    Rc<DxvkResourceAllocation> allocation;
    VkDeviceSize offset = 0, size = 0;
  };

  // Plan both slices before allocating. Hits pin the earlier slice; only the
  // misses advance the account. A failed second allocation commits neither.
  bool reserve(scene::CasterCopySizes& writes, const scene::CasterSliceKey& vertexKey,
      const scene::CasterSliceKey& indexKey, Slice& vertex, Slice& index, const scene::CasterLocalBounds& world,
      const scene::CasterQuantization* quant, const uint8_t*& indices, uint32_t indexMinimum) {
    budgetMiss = false;
    if (!writes.vertex && !writes.index)
      return true;
    const bool sameEncoding = m_quantized == bool(quant) && (!quant || quant->box == m_quantBox);
    const bool vertexHit = writes.vertex && sameEncoding && m_vertex.find(vertexKey, vertex);
    bool indexHit = writes.index && m_index.find(indexKey, index);
    unsigned bucket = 0;
    uint64_t hash = 0;
    const bool lookup = indices && !indexHit && writes.index && writes.index <= limit;
    if (lookup) {
      indices = scene::RebaseCasterIndices(m_rebased, indices, indexKey.count, indexMinimum);
      hash = scene::CasterIndexHash(indices, size_t(writes.index));
      bucket = m_content.find(hash, indices, size_t(writes.index), index);
      indexHit = index.buffer != nullptr;
    }
    if (vertexHit) writes.vertex = 0;
    if (indexHit) writes.index = 0;
    scene::CasterCopyAccount next = m_account;
    scene::CasterCopyPlan plan;
    const bool fits = next.append(writes, limit, plan);
#ifdef _WIN64
    unsigned tier = 0;
    const bool allowed = admission ? admission->allows(writes, world, limit, fits, next, tier) : fits;
#else
    (void)world;
    const bool allowed = fits;
#endif
    if (!allowed) {
      budgetMiss = true;
      return false;
    }
    Rc<DxvkBuffer> chunk = m_chunk;
    if ((writes.vertex && !reserveSlice(plan.vertex, writes.vertex, chunk, vertex)) ||
        (writes.index && !reserveSlice(plan.index, writes.index, chunk, index)))
      return false;
#ifdef _WIN64
    if (admission) admission->accepted(tier, writes.vertex + writes.index);
#endif
    m_chunk = std::move(chunk);
    m_account = next;
    if (writes.vertex && vertexKey.generation) {
      m_vertex = {vertexKey, vertex};
      m_quantized = quant != nullptr;
      if (quant) m_quantBox = quant->box;
    }
    if (lookup && writes.index)
      m_content.insert(bucket, hash, indices, size_t(writes.index), index);
    vertexBytes += writes.vertex;
    indexBytes += writes.index;
    indexReusedBytes += indexHit ? index.size : 0;
    if (writes.index && indexKey.generation) m_index = {indexKey, index};
    vertexHits += vertexHit;
    indexHits += indexHit;
    reusedBytes += (vertexHit ? vertex.size : 0u) + (indexHit ? index.size : 0u);
    return true;
  }

  uint64_t limit = scene::CasterCopyFrameBytes;
  bool budgetMiss = false, quantize = false;
  // Physical copies across all world scenes, consumed at the host Present.
  uint64_t vertexBytes = 0, indexBytes = 0, indexReusedBytes = 0;
  uint64_t reservedBytes() const { return m_account.frameBytes; }
#ifdef _WIN64
  scene::CasterAdmission* admission = nullptr;
#endif
  uint32_t vertexHits = 0, indexHits = 0;
  uint64_t reusedBytes = 0;
  scene::CasterBoundsCache bounds;

  void endFrame() {
    m_chunk = nullptr;
    m_account = {};
    m_vertex = {};
    m_index = {};
    m_content.clear();
    bounds = {};
    vertexHits = indexHits = 0;
    reusedBytes = 0;
  }

private:
  bool reserveSlice(const scene::CasterPlacement& place, uint64_t size, Rc<DxvkBuffer>& chunk, Slice& slice) {
    if (place.newChunk) {
      DxvkBufferCreateInfo info;
      info.size = place.newChunk;
      info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
      info.stages = VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
      info.access = VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT;
      info.debugName = "War3ShadowCasterPool";
      try {
        chunk = m_device->createBuffer(info,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      } catch (const DxvkError&) {
        // A lost device is not a full pool.
        if (m_device->getDeviceStatus() != VK_SUCCESS)
          throw;
        return false;
      }
      if (chunk == nullptr || chunk->mapPtr(0) == nullptr)
        return false;
    }
    slice.buffer = chunk;
    slice.allocation = chunk->storage();
    slice.offset = place.offset;
    slice.size = size;
    return true;
  }

  bool m_quantized = false;
  scene::CasterBox m_quantBox{};
  std::vector<uint16_t> m_rebased;
  scene::CasterIndexTable<Slice> m_content;
  scene::CasterCopyAccount m_account;
  scene::CasterRecentSlice<Slice> m_vertex, m_index;
  Rc<DxvkDevice> m_device;
  Rc<DxvkBuffer> m_chunk;
};

// ---- casters -----------------------------------------------------------------

// The geometry of a caster draw, as the executor holds it. Offsets are bytes
// from the start of the DxvkBuffer.
struct ShadowCasterGeometry {
  float admissionBase = scene::CasterPartBase::unknown;
  Rc<DxvkBuffer> vertexBuffer;        // stream 0
  VkDeviceSize vertexOffset = 0;      // of the stream in the buffer
  uint32_t stride = 0, positionOffset = 0;
  bool positionFloat3 = false;        // the declaration has POSITION0 as three floats
  Rc<DxvkBuffer> indexBuffer;         // null: not indexed
  VkDeviceSize indexOffset = 0;
  VkIndexType indexType = VK_INDEX_TYPE_UINT16;
  bool triangleList = false;
  // The draw call's own numbers.
  int32_t baseVertexIndex = 0;
  uint32_t minVertexIndex = 0, numVertices = 0, startIndex = 0, startVertex = 0, primitiveCount = 0;
  scene::CasterIndexSpan frozenIndices;
  // A dynamic buffer may be rewritten before the pass replays it. Its bytes as
  // the draw read them, from the start of the buffer (as the offsets above):
  // the executor copies the part the draw reads into its pool.
  bool vertexDynamic = false, indexDynamic = false;
  // Host-only facts. The in-process fixture keeps its original vertex layout.
  bool compact = false;
  uint64_t readableVertexBytes = UINT64_MAX;
  uint32_t vertexSlot = 0, indexSlot = 0;
  uint64_t vertexGeneration = 0, indexGeneration = 0;
  const uint8_t* vertexData = nullptr;
  const uint8_t* indexData = nullptr;
  // An alpha-tested caster (ShadowCasterAlpha): the record's threshold, the
  // texture the draw bound at the record's stage, and the record's TEXCOORD
  // element of stream 0 (FindShadowTexcoord).
  bool alphaTest = false;
  float alphaRef = 0.5f;
  Rc<DxvkImageView> alphaTexture;
  uint32_t uvOffset = 0;
  bool uvFloat2 = false;
};

// Why the executor could not use a caster. Each one makes the frame's caster
// set incomplete (SealShadowScene).
enum class ShadowCasterRefusal : uint32_t {
  None = 0,
  // A dynamic buffer the executor could not copy: no bytes of it at the draw,
  // or no room left in the frame's pool.
  DynamicBuffer,
  // Not a triangle list of three-float object-space positions.
  Layout,
  // The draw reads outside its buffers.
  Range,
};

// POSITION0 of stream 0 in a vertex declaration.
inline bool FindShadowPosition(const D3DVERTEXELEMENT9* elements, uint32_t count, uint32_t& offset, bool& float3) {
  for (uint32_t i = 0; i < count; ++i) {
    const auto& e = elements[i];
    if (e.Stream == 0xff) break;
    if (e.Stream == 0 && e.Usage == D3DDECLUSAGE_POSITION && e.UsageIndex == 0) {
      offset = e.Offset; float3 = e.Type == D3DDECLTYPE_FLOAT3; return true;
    }
  }
  return false;
}

// TEXCOORD<usage> of stream 0 in a vertex declaration.
inline bool FindShadowTexcoord(const D3DVERTEXELEMENT9* elements, uint32_t count, uint32_t usage,
    uint32_t& offset, bool& float2) {
  for (uint32_t i = 0; i < count; ++i) {
    const auto& e = elements[i];
    if (e.Stream == 0xff) break;
    if (e.Stream == 0 && e.Usage == D3DDECLUSAGE_TEXCOORD && e.UsageIndex == usage) {
      offset = e.Offset; float2 = e.Type == D3DDECLTYPE_FLOAT2; return true;
    }
  }
  return false;
}

// What became of the frame's ShadowCaster records at the executor.
struct ShadowCasterAccount {
  uint32_t received = 0, refused = 0;
  uint32_t exactBounds = 0;
  std::array<uint32_t, 4> refusals{}; // by ShadowCasterRefusal
  // Casters the executor took from dynamic buffers, and the bytes it copied
  // for them into its pool.
  uint32_t copied = 0;
  uint64_t copiedBytes = 0;
  void note(ShadowCasterRefusal refusal, uint64_t bytesCopied = 0) {
    ++received;
    if (refusal != ShadowCasterRefusal::None) { ++refused; ++refusals[uint32_t(refusal)]; }
    else if (bytesCopied) { ++copied; copiedBytes += bytesCopied; }
  }
};

// The caster of one draw, without its identity: where its vertices and
// indices are, and how the draw reads them. Static geometry is handed over
// without a copy, as the device's capture does; what the draw read from a
// dynamic buffer is copied into the pool, and the caster replays the copy.
inline ShadowCasterRefusal ShadowCasterReplayGeometry(War3ShadowCasterDraw& draw, ShadowCasterPool& pool,
    const ShadowCasterGeometry& g, uint64_t& copied, const scene::ShadowCaster& record,
    scene::CasterLocalBounds& world) {
  if (g.vertexBuffer == nullptr || !g.triangleList || !g.positionFloat3 || !g.stride
      || g.positionOffset > g.stride || g.stride - g.positionOffset < 12u || !g.primitiveCount
      || g.primitiveCount > 0x00ffffffu) return ShadowCasterRefusal::Layout;
  const bool indexed = g.indexBuffer != nullptr;
  const uint64_t vertexBytes = g.vertexBuffer->info().size;
  if (g.vertexOffset > vertexBytes) return ShadowCasterRefusal::Range;
  const uint64_t streamBytes = vertexBytes - g.vertexOffset;
  const uint32_t elements = g.primitiveCount * 3u;
  // The vertices the draw can reference, in stream order.
  const uint32_t frozenCount = g.frozenIndices.count();
  if (frozenCount && (!indexed || !g.compact || !g.indexDynamic ||
      g.frozenIndices.min < g.minVertexIndex || g.frozenIndices.max >= uint64_t(g.minVertexIndex) + g.numVertices))
    return ShadowCasterRefusal::Range;
  const uint32_t minimum = frozenCount ? g.frozenIndices.min : g.minVertexIndex;
  const auto address = scene::CasterIndexedAddress::from(g.baseVertexIndex, minimum, g.vertexDynamic);
  const int64_t firstVertex = indexed ? address.firstVertex : int64_t(g.startVertex);
  const uint32_t vertexCount = frozenCount ? frozenCount : (indexed ? g.numVertices : elements);
  const uint64_t indexSize = g.indexType == VK_INDEX_TYPE_UINT32 ? 4u : 2u;
  const uint64_t indexBytes = indexed ? g.indexBuffer->info().size : 0u;
  if (indexed && (g.indexOffset > indexBytes
      || (uint64_t(g.startIndex) + elements) * indexSize > indexBytes - g.indexOffset))
    return ShadowCasterRefusal::Range;
  // An alpha-tested caster reads two-float coordinates from the same vertices,
  // and a texture.
  if (g.alphaTest && (g.alphaTexture == nullptr || !g.uvFloat2 || g.uvOffset > g.stride
      || g.stride - g.uvOffset < 8u))
    return ShadowCasterRefusal::Layout;

  // What the draw read from a dynamic buffer, as it read it: its vertices
  // from the first it can reference, its indices from the first it uses.
  const bool pack = g.compact && g.vertexDynamic;
  const uint32_t extent = g.alphaTest ? std::max(g.positionOffset + 12u, g.uvOffset + 8u) : g.positionOffset + 12u;
  // The last host vertex needs only its attributes. prepareDraw may have
  // validated a shader that did not read one of the caster's attributes.
  if (!scene::CasterVerticesReadable(std::min(vertexBytes, g.readableVertexBytes), g.vertexOffset,
      firstVertex, vertexCount, g.stride, g.compact ? extent : g.stride))
    return ShadowCasterRefusal::Range;
  const uint64_t vertexStart = g.vertexOffset + uint64_t(firstVertex) * g.stride;
  const uint64_t indexStart = g.indexOffset + uint64_t(g.startIndex) * indexSize;
  auto writes = scene::CasterCopySizes::forDraw(g.vertexDynamic ? vertexCount : 0u, g.alphaTest,
      indexed && g.indexDynamic ? elements : 0u, uint32_t(indexSize));
  if (g.vertexDynamic && !pack)
    writes.vertex = uint64_t(vertexCount) * g.stride;
  if ((writes.vertex && !g.vertexData) || (writes.index && !g.indexData))
    return ShadowCasterRefusal::DynamicBuffer;
  const scene::CasterSliceKey vertexKey = {g.vertexGeneration, vertexStart, g.vertexSlot, vertexCount,
      g.stride, g.positionOffset, g.alphaTest ? g.uvOffset : UINT32_MAX};
  const scene::CasterSliceKey indexKey = {g.indexGeneration, indexStart, g.indexSlot, elements, uint32_t(indexSize)};
  scene::CasterQuantization quant;
  bool quantized = false;
  if (pack) {
    const uint8_t* frozen = frozenCount && indexSize == 2 ? g.indexData + indexStart : nullptr;
    const auto localBounds = pool.bounds.get(vertexKey, indexKey, g.vertexData + vertexStart + g.positionOffset,
        frozen, minimum);
    world.known = scene::CasterWorldBounds(localBounds, record.world, world.box);
    quantized = pool.quantize && !g.alphaTest && world.known && quant.prepare(localBounds, record.world);
    if (quantized) writes.vertex = uint64_t(vertexCount) * 8;
  }
  ShadowCasterPool::Slice vertexCopy, indexCopy;
  auto priorityWorld = world;
  if (priorityWorld.known && std::isfinite(g.admissionBase))
    priorityWorld.box = scene::ExtendCasterBox(world.box, g.admissionBase);
  const bool rebase = frozenCount && indexSize == 2;
  const uint8_t* normalized = rebase ? g.indexData + indexStart : nullptr;
  if (!pool.reserve(writes, vertexKey, indexKey, vertexCopy, indexCopy, priorityWorld,
        quantized ? &quant : nullptr, normalized, minimum))
    return ShadowCasterRefusal::DynamicBuffer;
  if (writes.vertex) {
    auto* dst = static_cast<uint8_t*>(vertexCopy.buffer->mapPtr(vertexCopy.offset));
    if (quantized)
      quant.write(dst, g.vertexData + vertexStart + g.positionOffset, g.stride, vertexCount);
    else if (pack)
      scene::CopyCasterVertices(dst, g.vertexData + vertexStart, vertexCount, g.stride, g.positionOffset,
          g.alphaTest, g.uvOffset);
    else
      std::memcpy(dst, g.vertexData + vertexStart, size_t(writes.vertex));
  }
  if (writes.index)
    std::memcpy(indexCopy.buffer->mapPtr(indexCopy.offset), normalized ? normalized : g.indexData + indexStart, size_t(writes.index));

  draw.indexed = indexed;
  if (g.vertexDynamic) {
    // The copy starts at the first vertex the draw can reference.
    draw.positionStorage = vertexCopy.buffer;
    draw.positionPinnedAllocation = vertexCopy.allocation;
    draw.positionInfo = War3PinnedAllocationSliceInfo(draw.positionPinnedAllocation, vertexCopy.offset, vertexCopy.size);
  } else {
    draw.positionStorage = g.vertexBuffer;
    draw.positionPinnedAllocation = g.vertexBuffer->storage();
    draw.positionInfo = War3PinnedAllocationSliceInfo(draw.positionPinnedAllocation, g.vertexOffset, streamBytes);
  }
  draw.positionStride = quantized ? 8u : pack ? scene::CasterVertexStride(g.alphaTest) : g.stride;
  draw.positionOffset = pack ? 0u : g.positionOffset;
  draw.positionFormat = quantized ? VK_FORMAT_R16G16B16A16_UNORM : VK_FORMAT_R32G32B32_SFLOAT;
  const auto& replayWorld = quantized ? quant.world : record.world;
  for (uint32_t i = 0; i < 16; ++i) draw.worldMatrix[i/4][i%4] = replayWorld[i];
  draw.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  if (draw.positionInfo.buffer == VK_NULL_HANDLE) return ShadowCasterRefusal::Range;
  if (indexed) {
    if (g.indexDynamic) {
      // The copy starts at the first index the draw uses.
      draw.indexStorage = indexCopy.buffer;
      draw.indexPinnedAllocation = indexCopy.allocation;
      draw.indexInfo = War3PinnedAllocationSliceInfo(draw.indexPinnedAllocation, indexCopy.offset, indexCopy.size);
      draw.firstIndex = 0;
    } else {
      draw.indexStorage = g.indexBuffer;
      draw.indexPinnedAllocation = g.indexBuffer->storage();
      draw.indexInfo = War3PinnedAllocationSliceInfo(draw.indexPinnedAllocation, g.indexOffset, indexBytes - g.indexOffset);
      draw.firstIndex = g.startIndex;
    }
    if (draw.indexInfo.buffer == VK_NULL_HANDLE) return ShadowCasterRefusal::Range;
    draw.indexType = g.indexType;
    draw.indexCount = elements;
    // An index i reads vertex BaseVertexIndex + i of the stream; in a copy
    // starting at BaseVertexIndex + minimum that is i - minimum.
    draw.vertexOffset = rebase ? address.rebased(minimum) : address.replayBase;
    // Marker filters, far-cascade padding and the workload governor also read
    // the declared count. Keep their input and give the final fetch validator
    // the exact domain of the pinned IB, rebased onto the compact slice.
    draw.minVertexIndex = g.minVertexIndex;
    draw.numVertices = g.numVertices;
    if (frozenCount) {
      draw.shadowActualIndexDomainKnown = true;
      draw.shadowActualIndexMin = rebase ? 0u : g.frozenIndices.min;
      draw.shadowActualIndexMax = rebase ? frozenCount - 1u : g.frozenIndices.max;
    }
  } else {
    draw.vertexCount = elements;
    draw.firstVertex = g.vertexDynamic ? 0u : g.startVertex;
    draw.numVertices = elements;
  }
  if (g.alphaTest) {
    // The coordinates are in the position stream (binding 0), so they are
    // wherever its vertices are: the draw's buffer or the pool's copy.
    draw.alphaTestEnabled = true;
    draw.alphaRef = g.alphaRef;
    draw.diffuseTexture = g.alphaTexture;
    draw.uvFormat = VK_FORMAT_R32G32_SFLOAT;
    draw.uvOffset = pack ? 12u : g.uvOffset;
    draw.uvStride = draw.positionStride;
    draw.uvBinding = 0;
  }
  copied = writes.vertex + writes.index;
  return ShadowCasterRefusal::None;
}

// Appends the caster of a draw and its replay instance to the frame's scene,
// and notes it in the frame's account.
inline ShadowCasterRefusal AppendShadowCaster(War3FrameScene& scene, ShadowCasterAccount& account,
    ShadowCasterPool& pool, const scene::ShadowCaster& record, const ShadowCasterGeometry& g,
    uint64_t frameSerial, uint64_t mapEpoch, uint64_t deviceEpoch,
    scene::CasterLocalBounds* bounds = nullptr) {
  namespace flag = scene::ShadowCasterFlag;
  War3ShadowCasterDraw draw = {};
  uint64_t copied = 0;
  scene::CasterLocalBounds local;
  auto& world = bounds ? *bounds : local;
  pool.budgetMiss = false;
  const ShadowCasterRefusal refusal = ShadowCasterReplayGeometry(draw, pool, g, copied, record, world);
  account.note(refusal, copied);
  if (refusal != ShadowCasterRefusal::None)
    return refusal;
  draw.mapEpoch = mapEpoch;
  draw.deviceEpoch = deviceEpoch;
#ifdef _WIN64
  draw.proxyCaster = g.compact;
  if (g.compact && frameSerial && world.known) {
    draw.proxyWorldBounds = world.box;
    draw.proxyBoundsFrame = frameSerial;
    ++account.exactBounds;
  }
#endif

  draw.alphaBlendEnabled = (record.flags & flag::AlphaBlend) != 0;
  draw.depthWriteEnabled = (record.flags & flag::DepthWrite) != 0;
  draw.depthTestEnabled = (record.flags & flag::DepthTest) != 0;
  draw.additiveBlend = (record.flags & flag::Additive) != 0;
  draw.category = War3RenderState::StageCategory(record.category);
  draw.batchTag = War3BatchTag(record.batchTag);
  draw.stage = int16_t(record.stage);
  draw.batchHandle = record.batchHandle;
  draw.objectKind = record.objectKind;
  draw.pathBlocker = (record.flags & flag::PathBlocker) != 0;
  draw.pathBlockerGeometryMarker = (record.flags & flag::PathBlockerMarker) != 0;
  draw.rawcode = record.rawcode;
  draw.jHandle = record.objectHandle;
  // An identity that is compared with null and never followed.
  draw.shadowRenderablePart = reinterpret_cast<void*>(uintptr_t(record.part));
  draw.shadowExactGeometryKeyHash = record.geometryKey;
  draw.shadowUnitIdentityProven = (record.flags & flag::UnitIdentityProven) != 0;
  draw.shadowPartLifecycleState = War3ShadowPartLifecycleState(record.lifecycle);
  draw.boundsCenter = Vector4(record.boundsCenter[0], record.boundsCenter[1], record.boundsCenter[2], 1.0f);
  draw.boundsRadius = record.boundsRadius;
  draw.boundsProvenance = war3::render::War3ShadowBoundsProvenance(record.boundsProvenance);
  draw.boundsSourceGeneration = record.boundsGeneration;
  draw.boundsFrameSerial = record.boundsAge == scene::ShadowBoundsAgeUnknown || record.boundsAge >= frameSerial
      ? 0u : frameSerial - record.boundsAge;
  draw.boundsIdentityProven = (record.flags & flag::BoundsIdentityProven) != 0;
  draw.boundsSourceWasSkinned = (record.flags & flag::BoundsSourceWasSkinned) != 0;
  draw.boundsFrameLocalDynamic = (record.flags & flag::BoundsFrameLocalDynamic) != 0;
  draw.boundsAnimatedAttachment = (record.flags & flag::BoundsAnimatedAttachment) != 0;

  // The water casts nothing: it goes to the water's surface, not the casters.
  if (scene::IsWaterSurfaceRecord(record)) {
    Matrix4 world;
    for (uint32_t i = 0; i < 16; ++i) world[i / 4][i % 4] = record.world[i];
    scene.waterWorlds.push_back(world);
    scene.waterSurfaces.emplace_back(std::move(draw));
    return ShadowCasterRefusal::None;
  }

  War3ShadowInstanceRef instance = {};
  instance.replayDrawIndex = uint32_t(scene.shadowCasters.size());
  instance.batchHandle = draw.batchHandle;
  for (uint32_t i = 0; i < 16; ++i) instance.worldMatrix[i/4][i%4] = record.world[i];
  instance.boundsCenter = draw.boundsCenter;
  instance.boundsRadius = draw.boundsRadius;
  instance.category = draw.category;
  instance.batchTag = draw.batchTag;
  instance.objectKind = draw.objectKind;
  instance.mode = War3ShadowReplayMode::FixedWorld;
  scene.shadowInstances.emplace_back(std::move(instance));
  const bool indexed = draw.indexed;
  scene.shadowCasters.emplace_back(std::move(draw));
  ++scene.shadowStats.captured;
  ++(indexed ? scene.shadowStats.capturedIndexed : scene.shadowStats.capturedNonIndexed);
  return ShadowCasterRefusal::None;
}

// Ends the frame's caster set, as War3SealShadowProducerCompleteness does. The
// pass publishes a new shadow map only for a sealed set without omissions:
// what the data layer left out, what did not arrive, and what the executor
// refused all count. Without a ShadowFrame record the set stays unsealed.
inline void SealShadowScene(War3FrameScene& scene, const scene::ShadowPacket& packet,
    const ShadowCasterAccount& account, uint64_t frameSerial, uint64_t mapEpoch, uint64_t deviceEpoch) {
  for (auto& draw : scene.shadowCasters)
    War3CaptureShadowReplayBindings(draw, frameSerial, mapEpoch, deviceEpoch);
  for (auto& draw : scene.waterSurfaces)
    War3CaptureShadowReplayBindings(draw, frameSerial, mapEpoch, deviceEpoch);
  if (!packet.hasFrame) return;
  auto& completeness = scene.producerCompleteness;
  completeness.reasonMask |= packet.frame.omissionReasons;
  completeness.requiredCasterOmissionCount = War3SaturatingAddU64(
      completeness.requiredCasterOmissionCount, packet.frame.omittedCasters, completeness.counterOverflow);
  // Admission into replay storage is what the executor could not grant.
  const uint32_t missing = packet.frame.casters > account.received ? packet.frame.casters - account.received
      : account.received - packet.frame.casters;
  const uint64_t notInScene = uint64_t(account.refused) + missing;
  if (notInScene) {
    completeness.reasonMask |= uint32_t(War3RequiredCasterOmissionReason::ArenaAdmission);
    completeness.arenaAdmissionCount = War3SaturatingAddU64(
        completeness.arenaAdmissionCount, notInScene, completeness.counterOverflow);
    completeness.requiredCasterOmissionCount = War3SaturatingAddU64(
        completeness.requiredCasterOmissionCount, notInScene, completeness.counterOverflow);
  }
  scene.shadowStats.dynamicPoseSignature = packet.frame.dynamicPoseSignature;
  scene.shadowStats.dynamicPoseCount = packet.frame.dynamicPoseCount;
  scene.shadowStats.dynamicSkinnedOutputCount = packet.frame.dynamicSkinnedOutputCount;
  completeness.seal(frameSerial, mapEpoch, deviceEpoch);
}

// ---- the pass input ----------------------------------------------------------

// An alpha-tested caster's texture is read with the pass's own sampler, with
// the Lighting record's mip settings, as the device's capture does. Once the
// pass exists, before it runs.
inline void BindShadowAlphaSamplers(War3PipelineInput& input, War3ShadowReceiverPass& pass) {
  const auto& shadows = input.settings->shadows;
  for (auto& draw : input.scene.shadowCasters) {
    if (!draw.alphaTestEnabled || draw.diffuseTexture == nullptr)
      continue;
    draw.diffuseSampler = pass.getFallbackSampler(shadows.alphaShadowUseMip, shadows.alphaShadowMipLodBias);
    if (draw.diffuseSampler != nullptr)
      draw.diffuseSamplerIndex = draw.diffuseSampler->getDescriptor().samplerIndex;
    draw.textureDescriptor = *draw.diffuseTexture->getDescriptor();
  }
}

// ---- the weather medium ---------------------------------------------------------

// The volumetric light pass also scatters the weather's haze and low snow
// (War3PipelineInput::weather), with or without a VolumetricLight record. In
// the game the pipeline takes the weather runtime's snapshot every frame; an
// executor that runs from records takes the part the pass reads from the
// frame's Snow record (war3_effect_scene.h), in its own frame and epochs.
// The runtime's revisions are not on the wire: the weather medium keeps no
// history (its froxel temporal filter is off), and they stay 0.
inline war3::render::WeatherFrameSnapshot WeatherFromSnow(const scene::Snow& s, uint64_t frameSerial,
    uint64_t mapEpoch, uint64_t deviceEpoch) {
  war3::render::WeatherFrameSnapshot weather;
  weather.params.snowfall = s.snowfall;
  weather.params.gust = s.gust;
  weather.params.haze = s.haze;
  weather.params.lowSnow = s.lowSnow;
  weather.region.minX = s.minX;
  weather.region.minY = s.minY;
  weather.region.maxX = s.maxX;
  weather.region.maxY = s.maxY;
  weather.region.bottomZ = s.bottomZ;
  weather.region.topZ = s.topZ;
  weather.region.edgeFeather = s.edgeFeather;
  weather.timeSeconds = s.timeSeconds;
  weather.windDisplacementX = s.windDisplacementX;
  weather.windDisplacementY = s.windDisplacementY;
  weather.seed = s.seed;
  weather.id = s.id;
  weather.frameSerial = frameSerial;
  weather.mapEpoch = mapEpoch;
  weather.deviceEpoch = deviceEpoch;
  weather.enabled = true;
  return weather;
}

// The frame has a weather medium for the volumetric light pass.
inline bool WeatherMedium(const scene::FramePacket& packet) {
  return packet.hasSnow && packet.snow.id > 0 && (packet.snow.haze > 0.0f || packet.snow.lowSnow > 0.0f);
}

// The volumetric light pass runs after the shadow pass: for the authored light
// (a VolumetricLight record) or for the weather medium.
inline bool VolumetricRuns(const scene::FramePacket& packet, const scene::ShadowPacket& shadow) {
  return shadow.hasVolumetric || WeatherMedium(packet);
}

// The scene moves into the input: the frame's casters are replayed once.
inline War3PipelineInput ShadowPipelineInput(const Rc<DxvkImageView>& color, const Rc<DxvkImageView>& depth,
    const scene::FramePacket& packet, const scene::ShadowPacket& shadow, War3FrameScene&& scene,
    uint32_t frameIndex, uint64_t frameSerial, uint64_t mapEpoch, uint64_t deviceEpoch,
    const War3CasterClasses& classes = {}, const War3SunStyleSettings& sunStyle = {},
    const War3WaterSettings& water = {}) {
  War3PipelineInput input;
  input.colorView = color;
  input.depthView = depth;
  input.scene = std::move(scene);
  if (packet.hasCamera)
    input.scene.worldCamera = ShadowWorldCamera(packet.camera, frameIndex, frameSerial);
  input.frameIndex = frameIndex;
  input.frameSerial = frameSerial;
  input.mapEpoch = mapEpoch;
  input.deviceEpoch = deviceEpoch;
  input.settings = ShadowPassSettings(shadow.lighting, shadow.hasOutline ? &shadow.outline : nullptr,
      shadow.hasVolumetric ? &shadow.volumetric : nullptr, classes, sunStyle, water);
  input.lighting = std::make_shared<War3FrameLightingState>();
  if (packet.hasSnow)
    input.weather = WeatherFromSnow(packet.snow, frameSerial, mapEpoch, deviceEpoch);
  return input;
}

// ---- the data layer's facts ----------------------------------------------------

// A ShadowFrame record carries four facts of the x86 data layer as they stood
// when the frame was sealed: the game's time of day, the caster tombstone
// serial, the stage policy revision and the runtime modules that are off. The
// pass does not take them from its input. It asks the data layer, through
//   War3RenderState::GetGameTime
//   war3::render::CurrentShadowCasterTombstoneSerial
//   war3::render::CurrentShadowStagePolicyRevision
//   war3::runtime::IsWar3RuntimeModuleEnabled
// so an executor that runs from records puts the record's facts where those
// functions read them, before the run:
//   x64 host:    its own definitions of the four (shadow_host_services.cpp);
//   in process:  the x86 data layer itself (d3d9_war3_scene_effects_fixture.cpp).
// A frame without the record leaves the facts as they were. The serial and
// the revision are compared with what the pass last saw, for change only.

// The runtime modules the pass asks about. Of a record's module word only
// these bits reach it.
inline constexpr uint32_t ShadowPassModules =
    1u << uint32_t(war3::runtime::War3RuntimeModule::ShadowTaa);

// ---- point lights ------------------------------------------------------------

// The pass reads the frame's point lights from the data layer's light manager
// (War3LightManager::GetFrameSnapshot). An executor that runs from records puts
// the PointLights record there before the run - the x64 host into its own
// manager, in process into the x86 data layer's - and a frame without the
// record leaves it empty. The manager changes only when the lights differ from
// what it holds, as the game changes it only when a light does: the pass keys
// what it keeps of earlier frames on the manager's generation and light ids.
inline bool SamePointLight(const War3PointLight& held, const scene::PointLight& wanted) {
  return held.active && held.position.x == wanted.position[0] && held.position.y == wanted.position[1]
      && held.position.z == wanted.position[2] && held.position.w == std::max(1.0f, wanted.range)
      && held.color.x == wanted.color[0] && held.color.y == wanted.color[1] && held.color.z == wanted.color[2]
      && held.color.w == wanted.intensity && held.params.x == wanted.shadowIntensity;
}

inline void ApplyPointLights(const scene::ShadowPacket& packet) {
  auto& manager = War3LightManager::Instance();
  const scene::PointLights none;
  const scene::PointLights& wanted = packet.hasPointLights ? packet.pointLights : none;
  const std::vector<War3PointLight> held = manager.GetActiveLights();
  bool same = held.size() == wanted.count;
  for (uint32_t i = 0; same && i < wanted.count; ++i)
    same = SamePointLight(held[i], wanted.lights[i]);
  if (same)
    return;
  manager.ClearLights();
  for (uint32_t i = 0; i < wanted.count; ++i) {
    const scene::PointLight& l = wanted.lights[i];
    manager.AddPointLight(l.position[0], l.position[1], l.position[2], l.range,
        l.color[0], l.color[1], l.color[2], l.intensity, l.shadowIntensity);
  }
}

// ---- the unit outline's objects -------------------------------------------------

// The unit outline draws the casters whose batchHandle the data layer names
// (War3RenderState::IsOutlineHandle). An executor that runs from records puts
// the Outline record's handles there before the run - the x64 host into its
// own set (shadow_host_services.cpp), in process into the x86 data layer's -
// and a frame without the record leaves none. The set changes only when the
// record's handles differ from it.
inline void ApplyOutlineHandles(const scene::ShadowPacket& packet) {
  const uint32_t count = packet.hasOutline ? packet.outline.handleCount : 0u;
  bool same = War3RenderState::GetOutlineHandleCount() == count;
  for (uint32_t i = 0; same && i < count; ++i)
    same = War3RenderState::IsOutlineHandle(packet.outline.handles[i]);
  if (same)
    return;
  War3RenderState::ClearOutlineHandles();
  for (uint32_t i = 0; i < count; ++i)
    War3RenderState::AddOutlineHandle(packet.outline.handles[i]);
}

// ---- the map's fog volumes -------------------------------------------------------

// The volumetric light pass reads the fog volumes from the data layer's
// manager (War3FogVolumeManager::GetFrameSnapshot). An executor that runs from
// records builds them there from the FogVolumes record before the run - the
// x64 host in its own manager, in process in the x86 data layer's - through
// the manager's own interface, so the volumes the pass reads are derived by
// the same code at both ends. A frame without the record leaves none. The
// manager is rebuilt only when the record differs from the one last built or
// something else changed the manager since.
inline bool SameFogVolume(const scene::FogVolume& a, const scene::FogVolume& b) {
  return a.shape == b.shape && a.active == b.active && a.position == b.position && a.halfSize == b.halfSize
      && a.rotationDegrees == b.rotationDegrees && a.density == b.density && a.edgeFeather == b.edgeFeather;
}

inline void ApplyFogVolumes(const scene::ShadowPacket& packet) {
  struct Built { scene::FogVolumes volumes; uint64_t generation = 0; bool valid = false; };
  static std::mutex mutex;
  static Built built;
  const scene::FogVolumes none;
  const scene::FogVolumes& wanted = packet.hasFogVolumes ? packet.fogVolumes : none;
  auto& manager = War3FogVolumeManager::Instance();
  std::lock_guard<std::mutex> lock(mutex);
  bool same = built.valid && built.generation == manager.GetGeneration() && built.volumes.count == wanted.count;
  for (uint32_t i = 0; same && i < wanted.count; ++i)
    same = SameFogVolume(built.volumes.volumes[i], wanted.volumes[i]);
  if (same)
    return;
  manager.Clear();
  for (uint32_t i = 0; i < wanted.count; ++i) {
    const scene::FogVolume& v = wanted.volumes[i];
    const auto& at = v.position;
    const auto& half = v.halfSize;
    int32_t id = 0;
    if (v.shape == uint32_t(War3FogVolumeShape::Sphere))
      id = manager.AddSphere(at[0], at[1], at[2], half[0], v.density, v.edgeFeather);
    else if (v.shape == uint32_t(War3FogVolumeShape::Box))
      id = manager.AddBox(at[0], at[1], at[2], 2.0f * half[0], 2.0f * half[1], 2.0f * half[2],
          v.density, v.edgeFeather);
    else
      id = manager.AddCylinder(at[0], at[1], at[2], half[0], 2.0f * half[2], v.density, v.edgeFeather);
    if (id == 0)
      continue;
    const auto& r = v.rotationDegrees;
    if (r[0] != 0.0f || r[1] != 0.0f || r[2] != 0.0f)
      manager.SetRotationDegrees(id, r[0], r[1], r[2]);
    if (v.active == 0u)
      manager.SetActive(id, false);
  }
  built.volumes = wanted;
  built.generation = manager.GetGeneration();
  built.valid = true;
}

// ---- what a run did ----------------------------------------------------------

// The pass's own account of its last Run, for comparing two executors.
struct ShadowRunFacts {
  uint32_t casters = 0, replayDraws = 0, drawnCasters = 0, cascades = 0;
  std::array<uint32_t, 4> cascadeDrawn{};
  uint32_t mapExecuted = 0, receiverExecuted = 0;
  // ReceiverRunEarlyReturnReason, ReceiverInputRejectReason, replay refusals.
  uint32_t earlyReturn = 0, inputReject = 0, replayRejected = 0;
  // The receiver had a complete shadow map to draw with.
  uint32_t completeMap = 0;
  // Casters the shadow map was prepared to draw alpha-tested.
  uint32_t alphaPrepared = 0;
  // Shadow TAA: the module as the pass found it, and the mode it then ran in
  // (War3ShadowTaaMode).
  uint32_t taaModule = 0, taaMode = 0;
  // What the run did with the shadow TAA history (ShadowTaaHistoryBit), and
  // why a history was dropped (the pass's kShadowTaaInvalidate* bits).
  uint32_t taaHistory = 0, taaInvalidation = 0;
  // The receiver had point lights that cast shadows.
  uint32_t pointShadow = 0;
  // The run drew the unit outline: it was on, and the data layer named objects.
  uint32_t outline = 0;
  // The data layer's facts where the pass reads them, as they stand after the
  // run.
  float gameTime = 0.0f;
  uint64_t tombstoneSerial = 0, stagePolicyRevision = 0;
  // War3ShadowReceiverPass::ResourceBytes.
  uint64_t resourceBytes = 0;
  // The volumetric light pass, run after the shadow pass with its input when
  // the frame has a VolumetricLight record: it ran; the backend it drew with
  // (War3VolumetricQuality, -1 when it drew nothing); it composited into the
  // frame; War3VolumetricLightPass::ResourceBytes after the run (0 when it
  // did not run).
  uint32_t volumetric = 0, volumetricComposite = 0;
  int32_t volumetricBackend = -1;
  uint64_t volumetricBytes = 0;
  // 4d-3: the fog volumes the authored light scattered in, and whether the
  // weather medium (haze, low snow) was composited in the same run.
  uint32_t volumetricFogVolumes = 0, volumetricWeather = 0;
};

// ShadowRunFacts::taaHistory.
namespace ShadowTaaHistoryBit {
constexpr uint32_t MotionVectors = 1u << 0;   // the motion vector pass ran
constexpr uint32_t Visibility = 1u << 1;      // the current-frame visibility pass ran
constexpr uint32_t ValidBefore = 1u << 2;     // a history was there to read
constexpr uint32_t Written = 1u << 3;         // the receiver wrote a history
constexpr uint32_t Advanced = 1u << 4;        // and it became the next frame's
constexpr uint32_t Read = 1u << 5;            // the receiver blended the history in
}

// What the volumetric light pass did in the run that followed the shadow
// pass's. On the thread that ran it, or with that thread idle.
inline void AddVolumetricRunFacts(const War3VolumetricLightPass& pass, ShadowRunFacts& facts) {
  const War3VolumetricExecutionDiagnostics& last = pass.LastRun();
  facts.volumetric = 1u;
  facts.volumetricBackend = last.effectiveBackend;
  facts.volumetricComposite = last.compositeSubmitted ? 1u : 0u;
  facts.volumetricBytes = pass.ResourceBytes();
  facts.volumetricFogVolumes = last.fogVolumes;
  // The weather medium's owner keeps its own last run; it is this run's when
  // the frame numbers agree.
  const War3VolumetricExecutionDiagnostics* weather = pass.WeatherLastRun();
  facts.volumetricWeather = weather != nullptr && weather->frameSerial == last.frameSerial &&
      weather->mapEpoch == last.mapEpoch && weather->deviceEpoch == last.deviceEpoch &&
      weather->compositeSubmitted ? 1u : 0u;
}

// On the thread that ran the pass, or with that thread idle.
inline ShadowRunFacts ShadowRunFactsOf(const War3ShadowReceiverPass& pass) {
  const auto& r = pass.reconciliation;
  ShadowRunFacts facts;
  facts.casters = r.shadowCastersCount;
  facts.replayDraws = r.replayDrawsCount;
  facts.drawnCasters = r.shadowMapDrawnCasters;
  facts.cascades = r.receiverCsmCascadeCount;
  facts.cascadeDrawn = { r.shadowMapCascade0DrawnCount, r.shadowMapCascade1DrawnCount,
                         r.shadowMapCascade2DrawnCount, r.shadowMapCascade3DrawnCount };
  facts.mapExecuted = r.shadowMapExecutedThisFrame;
  facts.receiverExecuted = r.receiverDrawExecutedThisFrame;
  facts.earlyReturn = r.receiverRunEarlyReturnReason;
  facts.inputReject = r.receiverInputRejectReason;
  facts.replayRejected = r.replayValidationRejectedCount;
  facts.completeMap = r.receiverHasCompleteShadowMap;
  facts.alphaPrepared = r.shadowMapAlphaTestPreparedCount;
  namespace bit = ShadowTaaHistoryBit;
  facts.taaHistory = (r.shadowMotionVectorExecutedThisFrame ? bit::MotionVectors : 0u)
      | (r.shadowVisibilityExecutedThisFrame ? bit::Visibility : 0u)
      | (r.shadowHistoryValidBefore ? bit::ValidBefore : 0u)
      | (r.shadowHistoryWriteExecutedThisFrame ? bit::Written : 0u)
      | (r.shadowHistoryAdvancedThisFrame ? bit::Advanced : 0u)
      | (r.shadowTaaMode >= 3u ? bit::Read : 0u);
  facts.taaInvalidation = r.shadowHistoryInvalidationMask;
  facts.pointShadow = r.receiverHasPointShadow;
  facts.outline = r.receiverNeedOutlinePass;
  facts.taaModule = r.shadowTaaRuntimeModuleEnabled;
  facts.taaMode = r.shadowTaaEffectiveMode;
  facts.gameTime = War3RenderState::GetGameTime();
  facts.tombstoneSerial = war3::render::CurrentShadowCasterTombstoneSerial();
  facts.stagePolicyRevision = war3::render::CurrentShadowStagePolicyRevision();
  facts.resourceBytes = pass.ResourceBytes();
  return facts;
}

} // namespace dxvk::war3::effects
