#include "shadow_before_ui.h"
#include "shadow_host_services.h"
#include "../../../src/d3d9/war3/platform/war3_proxy_diag_block.h"

#include "../../../src/d3d9/war3/effects/war3_shadow_scene_input.h"
#include "../../../src/d3d9/d3d9_war3_water.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <utility>
#include <vector>

namespace warvk::host::dxvkcore {
namespace fx = dxvk::war3::effects;

// ---- the caster audit ---------------------------------------------------------
// For frame evidence (evidence_recorder.h), or WARVK_D3D9_CASTER_AUDIT=1: reads
// what every caster's draw reads - its indices, and the positions they name in
// the buffer as it is at the draw - and counts a draw whose indices leave
// D3D9's MinVertexIndex/NumVertices range, read outside the buffer, name a
// position that is not a number or far outside any map, or span a triangle
// longer than any model's. It changes nothing of the frame: the numbers go to
// the frame's outcome, and with the environment switch to the log as well.
namespace {
struct CasterAudit {
  bool forced = false;   // the environment switch: log lines too
  bool enabled = false;
  uint64_t casters = 0, odd = 0, logged = 0, frames = 0, peakFrameBytes = 0, peakFrameCasters = 0;
  ShadowOutcome::Audit frame;
  size_t oddUsed = 0;
  std::vector<float> world; // the positions of the caster in hand, three floats each
  CasterAudit() {
    char value[8] = {};
    const DWORD size = GetEnvironmentVariableA("WARVK_D3D9_CASTER_AUDIT", value, sizeof(value));
    forced = enabled = size == 1 && value[0] == '1';
  }
  void summary(const char* why) {
    if (!forced) return;
    std::fprintf(stderr, "D3D9 caster-audit: {\"why\":\"%s\",\"frames\":%llu,\"casters\":%llu,\"odd\":%llu,"
        "\"peakFrameBytes\":%llu,\"peakFrameCasters\":%llu}\n", why,
        static_cast<unsigned long long>(frames), static_cast<unsigned long long>(casters),
        static_cast<unsigned long long>(odd), static_cast<unsigned long long>(peakFrameBytes),
        static_cast<unsigned long long>(peakFrameCasters));
    std::fflush(stderr);
  }
  // The quick look that runs for every caster: the bounds of its positions and
  // of its indices. Whatever the full check below would count lies outside
  // these bounds, so a caster inside them is clean; any other goes on to the
  // full check, which alone measures triangle edges.
  bool looksClean(const fx::scene::ShadowCaster& record, const ShadowCasterSource& s, uint64_t first,
                  uint64_t vertices, uint64_t count, bool indexed, uint32_t minimum, uint32_t positionOffset) {
    const uint8_t* at = s.vertexData + s.streamOffset + first * s.stride + positionOffset;
    float lo[3] = { 3e38f, 3e38f, 3e38f }, hi[3] = { -3e38f, -3e38f, -3e38f };
    uint32_t special = 0;
    for (uint64_t v = 0; v < vertices; ++v) {
      float p[3];
      uint32_t bits[3];
      std::memcpy(p, at + v * s.stride, 12);
      std::memcpy(bits, at + v * s.stride, 12);
      for (uint32_t k = 0; k < 3; ++k) {
        special |= uint32_t((bits[k] & 0x7f800000u) == 0x7f800000u);
        lo[k] = std::min(lo[k], p[k]);
        hi[k] = std::max(hi[k], p[k]);
      }
    }
    if (special)
      return false;
    if (indexed) {
      const uint8_t* index = s.indexData + uint64_t(s.startIndex) * 2u;
      uint16_t lowest = 0xffffu, highest = 0u;
      for (uint64_t i = 0; i < count; ++i) {
        uint16_t value;
        std::memcpy(&value, index + i * 2u, 2);
        lowest = std::min(lowest, value);
        highest = std::max(highest, value);
      }
      if (lowest < minimum || uint64_t(highest) - minimum >= vertices)
        return false;
    }
    // The box of the transformed box (D3D9 row vectors: p * World).
    const float* w = record.world.data();
    float extentSquared = 0.0f, extent = 0.0f;
    for (uint32_t k = 0; k < 3; ++k) {
      float low = w[12 + k], high = w[12 + k];
      for (uint32_t r = 0; r < 3; ++r) {
        const float a = w[r * 4 + k] * lo[r], b = w[r * 4 + k] * hi[r];
        low += std::min(a, b);
        high += std::max(a, b);
      }
      if (!(std::abs(low) <= 40000.0f && std::abs(high) <= 40000.0f))
        return false;
      extentSquared += (high - low) * (high - low);
      extent = std::max(extent, high - low);
    }
    if (extentSquared > 3000.0f * 3000.0f)
      return false;
    frame.largestExtent = std::max(frame.largestExtent, extent);
    return true;
  }
  void caster(const fx::scene::ShadowCaster& record, const ShadowCasterSource& s, uint64_t frameSerial) {
    ++casters;
    uint32_t positionOffset = 0; bool float3 = false;
    if (!s.elements || !fx::FindShadowPosition(s.elements, s.elementCount, positionOffset, float3) || !float3) return;
    if (!s.vertexData || !s.triangleList || !s.stride || s.vertexBuffer == nullptr) return;
    const bool indexed = s.indexBuffer != nullptr;
    if (indexed && !s.indexData) return;
    ++frame.checked;
    const uint64_t vbBytes = s.vertexBuffer->info().size;
    const uint64_t count = uint64_t(s.primitiveCount) * 3u;
    // The vertices the draw may name, in stream order.
    const auto span = s.frozenIndices;
    const uint32_t minimum = span.count() ? span.min : s.minVertexIndex;
    const int64_t first = indexed ? int64_t(s.baseVertexIndex) + minimum : int64_t(s.startIndex);
    const uint32_t vertices = span.count() ? span.count() : (indexed ? s.numVertices : uint32_t(count));
    uint32_t outOfRange = 0, notNumber = 0, distant = 0, longEdges = 0;
    float lo[3] = { 3e38f, 3e38f, 3e38f }, hi[3] = { -3e38f, -3e38f, -3e38f }, longest = 0.0f;
    const bool inside = vertices <= (1u << 20)
        && fx::scene::CasterVerticesReadable(std::min(vbBytes, s.readableVertexBytes), s.streamOffset,
            first, vertices, s.stride, positionOffset + 12u)
        && (!indexed || (uint64_t(s.startIndex) + count) * 2u <= s.indexBuffer->info().size);
    if (inside && looksClean(record, s, uint64_t(first), vertices, count, indexed, minimum, positionOffset))
      return;
    if (inside) {
      world.resize(size_t(vertices) * 3u);
      const float* w = record.world.data();
      const uint8_t* at = s.vertexData + s.streamOffset + uint64_t(first) * s.stride + positionOffset;
      for (uint64_t v = 0; v < vertices; ++v) {
        float p[3];
        std::memcpy(p, at + v * s.stride, 12);
        float* out = world.data() + size_t(v) * 3u;
        if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) {
          ++notNumber;
          out[0] = out[1] = out[2] = 0.0f;
          continue;
        }
        // D3D9 row vectors: p * World.
        for (uint32_t k = 0; k < 3; ++k) {
          out[k] = p[0] * w[0 + k] + p[1] * w[4 + k] + p[2] * w[8 + k] + w[12 + k];
          lo[k] = std::min(lo[k], out[k]);
          hi[k] = std::max(hi[k], out[k]);
        }
        if (std::abs(out[0]) > 40000.0f || std::abs(out[1]) > 40000.0f || std::abs(out[2]) > 40000.0f) ++distant;
      }
      const float limit = 3000.0f * 3000.0f;
      const float* corner[3] = {};
      for (uint64_t i = 0; i < count; ++i) {
        uint64_t vertex = i; // not indexed: the vertices in order
        if (indexed) {
          uint16_t index = 0;
          std::memcpy(&index, s.indexData + (uint64_t(s.startIndex) + i) * 2u, 2);
          if (index < minimum || uint64_t(index) - minimum >= vertices) {
            ++outOfRange;
            vertex = UINT64_MAX;
          } else {
            vertex = uint64_t(index) - minimum;
          }
        }
        const uint32_t c = uint32_t(i % 3u);
        corner[c] = vertex == UINT64_MAX ? nullptr : world.data() + size_t(vertex) * 3u;
        if (c == 2u && corner[0] && corner[1] && corner[2]) {
          bool isLong = false;
          for (uint32_t e = 0; e < 3; ++e) {
            const float* a = corner[e]; const float* b = corner[(e + 1u) % 3u];
            const float d = (a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]);
            longest = std::max(longest, d);
            isLong = isLong || d > limit;
          }
          if (isLong) ++longEdges;
        }
      }
      longest = std::sqrt(longest);
      frame.longestEdge = std::max(frame.longestEdge, longest);
      if (lo[0] <= hi[0])
        frame.largestExtent = std::max(frame.largestExtent, std::max({ hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2] }));
    }
    if (outOfRange) ++frame.indexOutsideRange;
    if (!inside) ++frame.outsideBuffer;
    if (notNumber) ++frame.notNumber;
    if (distant) ++frame.farPosition;
    if (longEdges) ++frame.longEdge;
    if (!(outOfRange || !inside || notNumber || distant || longEdges)) return;
    ++odd;
    char line[400];
    const int length = std::snprintf(line, sizeof(line), "category=%u stage=%d kind=%u tag=%d triangles=%u base=%d min=%u num=%u start=%u "
        "indexOutside=%u inside=%d notNumber=%u far=%u longEdges=%u longest=%.0f box=[%.0f,%.0f,%.0f]-[%.0f,%.0f,%.0f] "
        "stride=%u origin=%.0f,%.0f,%.0f; ",
        record.category, int(record.stage), unsigned(record.objectKind), int(record.batchTag), s.primitiveCount,
        s.baseVertexIndex, s.minVertexIndex, s.numVertices, s.startIndex, outOfRange, int(inside), notNumber, distant,
        longEdges, double(longest), double(lo[0]), double(lo[1]), double(lo[2]), double(hi[0]), double(hi[1]), double(hi[2]),
        s.stride, double(record.world[12]), double(record.world[13]), double(record.world[14]));
    if (length > 0 && oddUsed + size_t(length) < frame.odd.size()) {
      std::memcpy(frame.odd.data() + oddUsed, line, size_t(length));
      oddUsed += size_t(length);
    }
    if (forced && logged < 80u) {
      ++logged;
      std::fprintf(stderr, "D3D9 caster-audit: frame=%llu %s\n", static_cast<unsigned long long>(frameSerial), line);
      std::fflush(stderr);
    }
  }
  // The frame's numbers, for its outcome; the next frame starts empty.
  ShadowOutcome::Audit take(const fx::ShadowCasterAccount& account, const fx::ShadowCasterPool& pool) {
    ++frames;
    peakFrameBytes = std::max<uint64_t>(peakFrameBytes, account.copiedBytes);
    peakFrameCasters = std::max<uint64_t>(peakFrameCasters, account.received);
    if (forced && frames % 3000u == 0u) summary("running");
    if (forced && frames % 120u == 0u) {
      std::fprintf(stderr, "D3D9 caster-tick: frame=%llu casters=%u bytes=%llu refused=%u vbHits=%u ibHits=%u reusedBytes=%llu\n",
          static_cast<unsigned long long>(frames), account.received,
          static_cast<unsigned long long>(account.copiedBytes), account.refused, pool.vertexHits, pool.indexHits,
          static_cast<unsigned long long>(pool.reusedBytes));
      std::fflush(stderr);
    }
    const ShadowOutcome::Audit result = frame;
    frame = ShadowOutcome::Audit();
    oddUsed = 0;
    return result;
  }
};
CasterAudit& Audit() { static CasterAudit audit; return audit; }
} // namespace

void ShadowBeforeUi::setCasterAudit(bool wanted) {
  Audit().enabled = Audit().forced || wanted;
}

namespace {
uint64_t CasterAdmissionLimit() {
  char value[64]{};
  const DWORD length = GetEnvironmentVariableA("WARVK_D3D9_CASTER_BUDGET_TEST_MIB", value, sizeof(value));
  if (!length || length >= sizeof(value)) return fx::scene::CasterCopyFrameBytes;
  char* end = nullptr;
  const int64_t parsed = std::strtoll(value, &end, 10);
  if (end == value || *end) return fx::scene::CasterCopyFrameBytes;
  const auto mib = std::clamp<int64_t>(parsed, 1, fx::scene::CasterCopyFrameBytes >> 20);
  std::fprintf(stderr, "WarVK proxy mode: caster admission test limit MiB=%lld\n", (long long)mib);
  return uint64_t(mib) << 20;
}
} // namespace

// Each slot has its own query pool. Only the owner and command lists retain
// it. Host reset is legal only after the last list using this slot retires.
class ShadowTimestampSlot : public dxvk::RcObject {
public:
  explicit ShadowTimestampSlot(const dxvk::Rc<dxvk::DxvkDevice>& device) : vk(device->vkd()) {
    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = 6;
    if (vk->vkCreateQueryPool(vk->device(), &info, nullptr, &pool) != VK_SUCCESS) pool = VK_NULL_HANDLE;
  }
  ~ShadowTimestampSlot() { if (pool) vk->vkDestroyQueryPool(vk->device(), pool, nullptr); }
  bool retired() { incRef(); return decRef() == 1; }
  dxvk::Rc<dxvk::vk::DeviceFn> vk;
  VkQueryPool pool = VK_NULL_HANDLE;
  unsigned mask = 0;
};

struct ShadowGpuTimers {
  struct Frame {
    uint64_t id = 0;
    size_t count = 0;
    bool missing = false;
    std::vector<dxvk::Rc<ShadowTimestampSlot>> scenes;
  };
  std::array<Frame, 8> frames;
  dxvk::Rc<ShadowTimestampSlot>* active = nullptr;
  double periodUs = 0;
  uint64_t mask = 0;
  bool initialized = false;

  void begin(const dxvk::Rc<dxvk::DxvkDevice>& device, uint64_t id) {
    active = nullptr;
    if (!initialized) {
      initialized = true;
      uint32_t count = 0;
      auto query = device->vki()->vkGetPhysicalDeviceQueueFamilyProperties;
      query(device->adapter()->handle(), &count, nullptr);
      std::vector<VkQueueFamilyProperties> properties(count);
      query(device->adapter()->handle(), &count, properties.data());
      const auto family = device->queues().graphics.queueFamily;
      const unsigned bits = family < count ? properties[family].timestampValidBits : 0;
      if (bits && bits <= 64) {
        mask = UINT64_MAX >> (64 - bits);
        periodUs = device->properties().core.properties.limits.timestampPeriod / 1000.0;
      }
    }
    if (!mask) return;
    auto& frame = frames[id % frames.size()];
    if (frame.id != id) {
      frame.id = id;
      frame.count = 0;
      frame.missing = false;
    }
    // A Present can contain several completed world scenes. Each records
    // into its own six queries; never time the other work between scenes.
    if (frame.count == frame.scenes.size())
      frame.scenes.emplace_back(new ShadowTimestampSlot(device));
    auto& slot = frame.scenes[frame.count++];
    if (!slot->pool || !slot->retired()) {
      frame.missing = true;
      return;
    }
    active = &slot;
    slot->mask = 0;
    slot->vk->vkResetQueryPool(device->handle(), slot->pool, 0, 6);
  }

  static void stamp(void* owner, const dxvk::Rc<dxvk::DxvkCommandList>& list, unsigned index) {
    auto& timers = *static_cast<ShadowGpuTimers*>(owner);
    if (!timers.active) return;
    const auto& slot = *timers.active;
    if (slot->mask & (1u << index)) return;
    if (!slot->mask) list->track(slot);
    list->cmdWriteTimestamp(dxvk::DxvkCmdBuffer::ExecBuffer,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, slot->pool, index);
    slot->mask |= 1u << index;
  }

  void read(uint64_t present, shadowhost::CasterBoundsCounts& out) {
    if (present <= 4) return;
    const uint64_t id = present - 4;
    const auto& frame = frames[id % frames.size()];
    if (frame.id != id || frame.missing) return;
    double total[2]{};
    bool ready[2]{true, true};
    for (size_t scene = 0; scene < frame.count; ++scene) {
      const auto& slot = frame.scenes[scene];
      if (!slot->mask) continue;
      uint64_t data[6][2]{};
      const auto result = slot->vk->vkGetQueryPoolResults(slot->vk->device(), slot->pool, 0, 6,
          sizeof(data), data, sizeof(data[0]), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
      if (result != VK_SUCCESS && result != VK_NOT_READY) return;
      for (unsigned i = 0; i < 6; i += 2) {
        if (!(slot->mask & (3u << i))) continue;
        const unsigned metric = i != 0;
        if (data[i][1] && data[i+1][1])
          total[metric] += double((data[i+1][0] - data[i][0]) & mask) * periodUs;
        else
          ready[metric] = false;
      }
    }
    out.completedTimingId = id;
    out.shadowMapGpuUs = ready[0] ? total[0] : 0;
    out.shadowReceiverGpuUs = ready[1] ? total[1] : 0;
  }
};

struct ShadowBeforeUi::State {
  explicit State(const dxvk::Rc<dxvk::DxvkDevice>& device) : device(device), pool(device) {
    pool.admission = &admission;
    char value[8]{};
    // Off unless WARVK_D3D9_CASTER_QUANTIZE=1: in game (LostTemple, (4)生与死,
    // 2026-11-19) it packed no vertex at all, so it has saved nothing yet.
    pool.quantize = GetEnvironmentVariableA("WARVK_D3D9_CASTER_QUANTIZE", value, sizeof(value)) == 1 && value[0] == '1';
    // Keep the record arrays across frames. Appending a caster or its omitted
    // box must not reallocate and copy the preceding records during a draw.
    scene.shadowCasters.reserve(fx::scene::CasterFrameRecordLimit);
    scene.shadowInstances.reserve(fx::scene::CasterFrameRecordLimit);
    scene.proxyOmittedBounds.reserve(fx::scene::CasterFrameRecordLimit);
    pool.limit = CasterAdmissionLimit();
  }
  dxvk::Rc<dxvk::DxvkDevice> device;
  // Created by the first run; a session without a Lighting record owns none.
  std::unique_ptr<dxvk::War3ShadowReceiverPass> pass;
  // Created by the first run with a VolumetricLight record or a weather
  // medium. It reads the shadow pass's maps, so it is destroyed before it
  // (declared after it).
  std::unique_ptr<dxvk::War3VolumetricLightPass> volumetric;
  // Created by the first frame with water in view and the water surface on.
  // It reads the shadow pass's water images, so it goes before it too.
  std::unique_ptr<dxvk::War3WaterSurfacePass> water;
  // The casters of the frame in flight, and the copies of what they read from
  // dynamic buffers.
  dxvk::War3FrameScene scene;
  fx::ShadowCasterAccount account;
  fx::ShadowCasterPool pool;
  uint64_t vertexHits = 0, indexHits = 0, reusedBytes = 0;
  uint32_t gamePid = 0;
  diag::CasterSettingsView classView;
  ShadowGpuTimers timers;
  uint64_t serial = 0, map = 0, deviceId = 0;
  uint64_t hintSerial = 0, hintMap = 0, hintDevice = 0;
  uint64_t picture = 0, lastRun = 0, lastPresent = 0;
  bool mixed = false;
  fx::scene::CasterAdmission admission;
  std::vector<fx::scene::OmittedCasterPart> omittedParts;
  void omit(const fx::scene::CasterLocalBounds& world, fx::scene::CasterClass cls,
            fx::scene::CasterPartBase part) {
    part.minimum = world.known ? world.box[2] : fx::scene::CasterPartBase::unknown;
    if (omittedParts.size() < fx::scene::CasterFrameRecordLimit)
      omittedParts.push_back({part, cls, world.known ? uint32_t(scene.proxyOmittedBounds.size()) : UINT32_MAX});
    scene.proxyBudgetOmission = true;
    if (world.known && scene.proxyOmittedBounds.size() < fx::scene::CasterFrameRecordLimit)
      scene.proxyOmittedBounds.push_back(world.box);
  }

  void clearFrame();
};

ShadowBeforeUi::ShadowBeforeUi(const dxvk::Rc<dxvk::DxvkDevice>& device, uint32_t gamePid)
  : m_state(std::make_unique<State>(device)) { m_state->gamePid = gamePid; }

ShadowBeforeUi::~ShadowBeforeUi() {
  Audit().summary("end");
  std::fprintf(stderr, "D3D9 caster-reuse: vbHits=%llu ibHits=%llu reusedBytes=%llu\n",
      static_cast<unsigned long long>(m_state->vertexHits + m_state->pool.vertexHits),
      static_cast<unsigned long long>(m_state->indexHits + m_state->pool.indexHits),
      static_cast<unsigned long long>(m_state->reusedBytes + m_state->pool.reusedBytes));
}

bool ShadowBeforeUi::layoutAgrees() {
  static const bool agrees = fx::LightingDefaultsAgree() && fx::VolumetricDefaultsAgree();
  return agrees;
}

void ShadowBeforeUi::addCaster(const fx::scene::ShadowCaster& record, const ShadowCasterSource& source,
    uint64_t frameSerial, uint64_t mapEpoch, uint64_t deviceEpoch) {
  if (Audit().enabled) Audit().caster(record, source, frameSerial);
  fx::ShadowCasterGeometry geometry;
  geometry.vertexBuffer = source.vertexBuffer;
  geometry.compact = true;
  geometry.readableVertexBytes = source.readableVertexBytes;
  geometry.vertexSlot = source.vertexSlot;
  geometry.vertexGeneration = source.vertexGeneration;
  geometry.indexSlot = source.indexSlot;
  geometry.indexGeneration = source.indexGeneration;
  geometry.vertexOffset = source.streamOffset;
  geometry.stride = source.stride;
  if (source.elements)
    fx::FindShadowPosition(source.elements, source.elementCount, geometry.positionOffset, geometry.positionFloat3);
  geometry.indexBuffer = source.indexBuffer;
  geometry.indexType = VK_INDEX_TYPE_UINT16;
  geometry.triangleList = source.triangleList;
  geometry.baseVertexIndex = source.baseVertexIndex;
  geometry.minVertexIndex = source.minVertexIndex;
  geometry.numVertices = source.numVertices;
  geometry.frozenIndices = source.frozenIndices;
  // One number in the draw message: the first index, or the first vertex.
  geometry.startIndex = source.startIndex;
  geometry.startVertex = source.startIndex;
  geometry.primitiveCount = source.primitiveCount;
  geometry.vertexDynamic = source.vertexDynamic;
  geometry.indexDynamic = source.indexDynamic;
  geometry.vertexData = source.vertexData;
  geometry.indexData = source.indexData;
  geometry.alphaTest = source.alphaTest;
  geometry.alphaRef = source.alphaRef;
  geometry.alphaTexture = source.alphaTexture;
  if (source.alphaTest && source.elements)
    fx::FindShadowTexcoord(source.elements, source.elementCount, source.texcoord, geometry.uvOffset, geometry.uvFloat2);
  auto& s = *m_state;
  if (!s.serial) {
    s.serial = frameSerial;
    s.map = mapEpoch;
    s.deviceId = deviceEpoch;
    if (s.hintSerial + 1 != frameSerial || s.hintMap != mapEpoch || s.hintDevice != deviceEpoch)
      s.admission.finish({});
  }
  s.mixed |= s.serial != frameSerial || s.map != mapEpoch || s.deviceId != deviceEpoch;
  const auto cls = fx::scene::ClassifyCaster(record.category, record.stage, record.objectKind,
      (record.flags & fx::scene::ShadowCasterFlag::UnitIdentityProven) != 0);
  const auto part = fx::scene::CasterBase(record.objectHandle ? record.objectHandle : record.batchHandle,
      record.world[12], record.world[13], record.world[14], fx::scene::CasterPartBase::unknown);
  if (s.pass && s.hintSerial + 1 == frameSerial && s.hintMap == mapEpoch && s.hintDevice == deviceEpoch)
    geometry.admissionBase = s.pass->proxyAdmissionBase(cls, part);
  fx::scene::CasterLocalBounds world;
  fx::AppendShadowCaster(s.scene, s.account, s.pool, record, geometry,
      frameSerial, mapEpoch, deviceEpoch, &world);
  if (s.pool.budgetMiss) s.omit(world, cls, part);
}

ShadowOutcome ShadowBeforeUi::run(const dxvk::Rc<dxvk::DxvkCommandList>& list,
    const dxvk::Rc<dxvk::DxvkImageView>& color, const dxvk::Rc<dxvk::DxvkImageView>& depth,
    const fx::scene::FramePacket& packet, const fx::scene::ShadowPacket& shadow,
    bool inPlaceWater, uint64_t& skippedWaterPasses,
    uint64_t frameSerial, uint64_t mapEpoch, uint64_t deviceEpoch, uint64_t presents,
    bool evidenceRing, shadowhost::CasterBoundsCounts& boundsCounts) {
  ShadowOutcome outcome;
  if (Audit().enabled)
    outcome.audit = Audit().take(m_state->account, m_state->pool);
  outcome.received = m_state->account.received;
  outcome.refused = m_state->account.refused;
  outcome.refusals = m_state->account.refusals;
  outcome.copied = m_state->account.copied;
  outcome.copiedBytes = m_state->account.copiedBytes;
  boundsCounts = {};
  auto& s = *m_state;
  const bool sameFrame = !s.mixed && (!s.serial || (s.serial == frameSerial && s.map == mapEpoch && s.deviceId == deviceEpoch));
  if (!sameFrame)
    s.scene.proxyOmittedBounds.clear();
  // Offscreen comparisons advance at BeforeUi. In the game, a stretch of
  // Presents without a shadow pass also consumes the old layers' hold time.
  s.picture += std::max(frameSerial - s.lastRun, presents - s.lastPresent);
  s.lastRun = frameSerial;
  s.lastPresent = presents;
  s.scene.proxyPictureSerial = s.picture;
  s.scene.proxyRecordFrame = true;
  s.scene.proxyBudgetOmission |= shadow.hasFrame && shadow.frame.omittedCasters != 0;
  boundsCounts.admittedBytes = s.pool.reservedBytes();
  boundsCounts.omittedExact = uint32_t(s.scene.proxyOmittedBounds.size());
  uint64_t unknown = outcome.refused - boundsCounts.omittedExact;
  if (shadow.hasFrame) {
    unknown += shadow.frame.omittedCasters;
    unknown += shadow.frame.casters > outcome.received ? shadow.frame.casters - outcome.received
        : outcome.received - shadow.frame.casters;
  }
  boundsCounts.omittedUnknown = uint32_t(std::min<uint64_t>(UINT32_MAX, unknown));
  boundsCounts.exact = m_state->account.exactBounds;
  boundsCounts.unknown = outcome.received - outcome.refused - boundsCounts.exact;
  if (!shadow.work()) { endFrame(); return outcome; }

  // The x86 data layer's facts of this frame, before anything of the pass
  // reads them (shadow_host_services.cpp). Without the record they stay.
  shadowhost::DataLayerFacts facts = shadowhost::Current();
  if (shadow.hasFrame) {
    facts.gameTime = shadow.frame.timeOfDay;
    facts.tombstoneSerial = shadow.frame.tombstoneSerial;
    facts.stagePolicyRevision = shadow.frame.stagePolicyRevision;
    facts.runtimeDisabledModules = shadow.frame.runtimeDisabledModules;
  }
  facts.frameIndex = uint32_t(frameSerial);
  shadowhost::Publish(facts);
  shadowhost::BeginFrame();
  // The frame's point lights, into this process's light manager, the objects
  // its unit outline draws, and the map's fog volumes.
  fx::ApplyPointLights(shadow);
  fx::ApplyOutlineHandles(shadow);
  fx::ApplyFogVolumes(shadow);

  if (!m_state->pass) {
    m_state->pass = std::make_unique<dxvk::War3ShadowReceiverPass>(m_state->device);
    char value[2]{};
    const bool enabled = GetEnvironmentVariableA("WARVK_D3D9_SHADOW_EXACT_CULL", value, sizeof(value)) != 1 ||
        value[0] != '0';
    m_state->pass->setProxyExactCull(enabled);
    std::fprintf(stderr, "D3D9 shadow exact cull: enabled=%u\n", unsigned(enabled));
  }
  m_state->pass->InvalidateMapEpoch(mapEpoch, deviceEpoch);

  s.classView.open(s.gamePid);
  dxvk::War3CasterClasses classes;
  diag::ReadCasterWords(classes, s.classView.read());
  dxvk::War3SunStyleSettings sunStyle;
  diag::DecodeSunStyle(sunStyle, s.classView.readSun());
  dxvk::War3WaterSettings water;
  diag::DecodeWater(water, s.classView.readWater());
  fx::SealShadowScene(m_state->scene, shadow, m_state->account, frameSerial, mapEpoch, deviceEpoch);
  dxvk::War3PipelineInput input = fx::ShadowPipelineInput(color, depth, packet, shadow,
      std::move(m_state->scene), uint32_t(frameSerial % 3u), frameSerial, mapEpoch, deviceEpoch, classes,
      sunStyle, water);
  fx::BindShadowAlphaSamplers(input, *m_state->pass);
  const bool measure = evidenceRing || s.classView.perfWanted();
  if (measure) s.timers.begin(s.device, presents + 1);
  s.pass->setProxyTimestampWriter(&s.timers, measure ? ShadowGpuTimers::stamp : nullptr);
  s.pass->proxyOmittedParts().swap(s.omittedParts);
  m_state->pass->Run(list, input);
  s.pass->proxyOmittedParts().swap(s.omittedParts);
  s.admission.finish(sameFrame ? s.pass->proxyFootprints() : fx::scene::CasterCascadeFootprints{});
  s.hintSerial = frameSerial;
  s.hintMap = mapEpoch;
  s.hintDevice = deviceEpoch;
  // The water surface (reflection, sun glint, the shadow on it) over the water
  // the shadow pass found this frame: after the shadows, before the
  // volumetric light, which lies over the water as over the land.
  const bool waterSurface = m_state->pass->waterFrame().valid && input.settings &&
                            input.settings->water.surfaceEnabled;
  if (waterSurface && inPlaceWater) ++skippedWaterPasses;
  if (waterSurface && !inPlaceWater) {
    if (!m_state->water)
      m_state->water = std::make_unique<dxvk::War3WaterSurfacePass>(m_state->device, m_state->pass.get());
    m_state->water->Run(list, input);
  }
  // The volumetric light, as the x86 device registers it: after the shadow
  // pass, with the same input - the maps it reads are this run's, and the sun
  // it lights with is the one the shadow pass resolved into input.lighting.
  // It runs for the authored light and for the weather medium.
  const bool volumetric = fx::VolumetricRuns(packet, shadow);
  if (volumetric) {
    if (!m_state->volumetric)
      m_state->volumetric = std::make_unique<dxvk::War3VolumetricLightPass>(m_state->device, m_state->pass.get());
    m_state->volumetric->Run(list, input);
  }

  fx::ShadowRunFacts run = fx::ShadowRunFactsOf(*m_state->pass);
  {
    // Frame evidence: what the pass decided about this frame's directional map.
    const auto& r = m_state->pass->reconciliation;
    boundsCounts.removed = r.proxyCsmRemoved;
    boundsCounts.complete = r.proxyComplete;
    boundsCounts.held = r.proxyHeld;
    boundsCounts.unavailable = r.proxyUnavailable;
    if (Audit().forced)
      std::fprintf(stderr, "D3D9 caster-layers: {\"complete\":%u,\"held\":%u,\"unavailable\":%u,"
          "\"omittedExact\":%u,\"omittedUnknown\":%u,\"bytes\":%llu}\n",
          boundsCounts.complete, boundsCounts.held, boundsCounts.unavailable, boundsCounts.omittedExact,
          boundsCounts.omittedUnknown, static_cast<unsigned long long>(boundsCounts.admittedBytes));
    outcome.reuseMap = r.receiverReuseShadowMap;
    outcome.holdInvalidCsm = r.receiverHoldInvalidCsmCount;
    outcome.holdEmptyReplay = r.receiverHoldEmptyReplayCount;
    outcome.holdIdentityChurn = r.receiverHoldIdentityChurnCount;
    outcome.csmFallback = r.receiverCsmFallbackToLastGoodCount;
    outcome.noCompleteMap = r.receiverNoCompleteShadowMapCount;
    outcome.replayLastReason = r.replayValidationLastReason;
    outcome.partialPrevented = r.replayPartialPreventedCount;
    outcome.skippedNoResources = r.shadowMapRenderSkippedNoResourcesCount;
    outcome.cameraHash = r.receiverCameraHash;
    outcome.sunHash = r.receiverSunDirectionHash;
    outcome.csmHash = r.receiverCsmHash;
    outcome.cameraDelta = r.receiverCameraDeltaNano;
    outcome.sunDelta = r.receiverSunDeltaNano;
    outcome.csmDelta = r.receiverCsmDeltaNano;
    outcome.snappedCenterDelta = r.receiverSnappedCenterDeltaTexelsNano;
    outcome.texelSizeDelta = r.receiverTexelSizeDeltaNano;
    outcome.mapRenderSerial = r.shadowMapRenderSerial;
  }
  if (volumetric)
    fx::AddVolumetricRunFacts(*m_state->volumetric, run);
  if (m_state->water)
    run.resourceBytes += m_state->water->ResourceBytes();
  outcome.ran = true;
  outcome.casters = run.casters; outcome.replayDraws = run.replayDraws;
  outcome.drawnCasters = run.drawnCasters; outcome.cascades = run.cascades;
  outcome.cascadeDrawn = run.cascadeDrawn;
  outcome.mapExecuted = run.mapExecuted; outcome.receiverExecuted = run.receiverExecuted;
  outcome.earlyReturn = run.earlyReturn; outcome.inputReject = run.inputReject;
  outcome.replayRejected = run.replayRejected;
  outcome.completeMap = run.completeMap; outcome.taaModule = run.taaModule; outcome.taaMode = run.taaMode;
  outcome.alphaPrepared = run.alphaPrepared;
  outcome.taaHistory = run.taaHistory; outcome.taaInvalidation = run.taaInvalidation;
  outcome.pointShadow = run.pointShadow; outcome.outline = run.outline;
  outcome.volumetric = run.volumetric; outcome.volumetricComposite = run.volumetricComposite;
  outcome.volumetricBackend = run.volumetricBackend; outcome.volumetricBytes = run.volumetricBytes;
  outcome.volumetricFogVolumes = run.volumetricFogVolumes; outcome.volumetricWeather = run.volumetricWeather;
  outcome.gameTime = run.gameTime; outcome.tombstoneSerial = run.tombstoneSerial;
  outcome.stagePolicyRevision = run.stagePolicyRevision; outcome.resourceBytes = run.resourceBytes;
  s.scene = std::move(input.scene);
  s.clearFrame();
  return outcome;
}

void ShadowBeforeUi::State::clearFrame() {
  // Casters of a frame that never ran the pass are not the next frame's.
  if (Audit().enabled && Audit().frame.checked) {
    Audit().frame = ShadowOutcome::Audit();
    Audit().oddUsed = 0;
  }
  omittedParts.clear();
  scene.shadowCasters.clear();
  scene.shadowInstances.clear();
  scene.proxyOmittedBounds.clear();
  dxvk::War3FrameScene empty;
  empty.shadowCasters.swap(scene.shadowCasters);
  empty.shadowInstances.swap(scene.shadowInstances);
  empty.proxyOmittedBounds.swap(scene.proxyOmittedBounds);
  scene = std::move(empty);
  account = {};
  serial = 0;
  mixed = false;
  vertexHits += pool.vertexHits;
  indexHits += pool.indexHits;
  reusedBytes += pool.reusedBytes;
  pool.endFrame();
}

void ShadowBeforeUi::readPresentedCounters(uint64_t present, shadowhost::CasterBoundsCounts& counts) {
  counts.casterVertexBytes = std::exchange(m_state->pool.vertexBytes, 0);
  counts.casterIndexBytes = std::exchange(m_state->pool.indexBytes, 0);
  counts.casterIndexReusedBytes = std::exchange(m_state->pool.indexReusedBytes, 0);
  counts.timingId = present;
  m_state->timers.read(present, counts);
}

void ShadowBeforeUi::endFrame() {
  // FrameEnd/Present also arrive after a successful BeforeUi. Only a
  // discarded batch of draws invalidates its predecessor's demand history.
  if (m_state->serial) {
    m_state->hintSerial = 0;
    m_state->admission.finish({});
  }
  m_state->clearFrame();
}

} // namespace warvk::host::dxvkcore
