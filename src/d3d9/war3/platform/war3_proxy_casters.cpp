// war3_proxy_casters.cpp - see war3_proxy_casters.h

#include "war3_proxy_casters.h"

#include "war3_proxy_mode.h"
#include "../core/war3_internal_test_config.h"
#include "../core/war3_memory.h"
#include "../core/war3_game_structs.h"
#include "../war3.h"
#include "../core/war3_runtime_profile.h"
#include "../state/war3_render_state.h"
#include "../effects/war3_shadow_scene.h"
#include "../render/war3_current_draw_contract.h"
#include "../render/war3_render_objects.h"
#include "../render/war3_render_state.h"
#include "../render/war3_shadow_producer_policy.h"
#include "../../d3d9_war3_hook.h"
#include "../../d3d9_war3_light.h"
#include "../../d3d9_war3_scene.h"
#include "../../d3d9_war3_settings.h"
#include "../../../util/log/log.h"
#include "../../../util/util_bit.h"
#include "../../../util/util_env.h"
#include "../../../util/util_string.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace dxvk::war3::proxy {

namespace {

namespace scene = effects::scene;
using render::ObjectKind;
using Category = War3RenderState::StageCategory;
static_assert(uint32_t(Category::Terrain) == 2 && uint32_t(Category::WorldObject) == 3);
static_assert(uint32_t(ObjectKind::Unit) == 1 && uint32_t(ObjectKind::Building) == 2 &&
    uint32_t(ObjectKind::Destructible) == 3 && uint32_t(ObjectKind::Item) == 4 && uint32_t(ObjectKind::Effect) == 5);

enum CasterClass : uint32_t { ClassTerrain = 0, ClassDoodad, ClassWorldObject, ClassUnit, ClassCount };

struct State {
  bool wanted = false;
  bool outlineWanted = false;
  bool objectClasses = false; // a class is set apart: classify by game object
  War3CasterClasses classes{};
  uint32_t sent = 0, dynamicCasters = 0;
  uint64_t signature = 0;
  uint64_t frame = 0;
  scene::CasterPoolMirror pool;
  CasterStatus status;
  CasterFrameCounts frameCounts;
  std::array<uint32_t, ClassCount> logged{};
};

State c;

void Skip(CasterSkip why) { ++c.status.skipped[uint32_t(why)]; }

DWORD RenderStateOf(IDirect3DDevice9* device, D3DRENDERSTATETYPE state) {
  DWORD value = 0;
  return SUCCEEDED(device->GetRenderState(state, &value)) ? value : 0u;
}

bool IsBlocker(uint32_t rawcode) {
  return rawcode != 0u && internal::IsPathBlockerFourCc(rawcode);
}

// A widget of the game, read defensively: its type code when it is a
// pathing blocker's.
//
// Each defensive read is a VirtualQuery, a system call, and a frame asks about
// the same few hundred widgets once per draw of their models: 6.5% of the game
// thread on a heavy map (profile of 2026-11-18). So the answer is kept per
// widget for the frame it was read in, and the two fields are covered by one
// range check. A widget cannot turn into a blocker between two draws of one
// frame; the next frame reads it again.
bool WidgetIsBlocker(void* widget) {
  if (widget == nullptr)
    return false;
  struct Memo { void* widget; uint64_t frame; bool blocker; };
  static std::array<Memo, 512> memo{};
  const auto address = reinterpret_cast<uintptr_t>(widget);
  Memo& slot = memo[((address >> 4) ^ (address >> 13)) & (memo.size() - 1u)];
  // c.frame starts at 1 (CastersBeginFrame), so an empty slot never matches.
  if (slot.widget == widget && slot.frame == c.frame)
    return slot.blocker;
  uint32_t magic = 0u, rawcode = 0u;
  bool read = false;
  if (IsReadableRangeFast(widget, 0x34u)) {
    std::memcpy(&magic, static_cast<const uint8_t*>(widget) + 0x0Cu, sizeof(magic));
    std::memcpy(&rawcode, static_cast<const uint8_t*>(widget) + 0x30u, sizeof(rawcode));
    read = true;
  } else {
    // The object lies across two regions: field by field, as before.
    read = SafeReadU32Fast(widget, 0x0Cu, magic) && magic == 0x2B5DB42Cu &&
           SafeReadU32Fast(widget, 0x30u, rawcode);
  }
  const bool blocker = read && magic == 0x2B5DB42Cu && IsBlocker(rawcode);
  slot = Memo{widget, c.frame, blocker};
  return blocker;
}

// What the draw's vertex declaration says about stream 0.
struct Layout {
  bool position = false, positionFloat3 = false;
  uint32_t positionOffset = 0;
  bool texcoord = false, texcoordFloat2 = false;
  uint32_t texcoordOffset = 0;
};

bool ReadLayout(IDirect3DDevice9* device, Layout& layout) {
  IDirect3DVertexDeclaration9* declaration = nullptr;
  if (FAILED(device->GetVertexDeclaration(&declaration)) || declaration == nullptr)
    return false;
  D3DVERTEXELEMENT9 elements[MAXD3DDECLLENGTH + 1];
  // In: the room in `elements` (the front end checks it); out: the elements.
  UINT count = MAXD3DDECLLENGTH + 1;
  const HRESULT hr = declaration->GetDeclaration(elements, &count);
  declaration->Release();
  if (FAILED(hr))
    return false;
  for (UINT i = 0; i < count; ++i) {
    const D3DVERTEXELEMENT9& e = elements[i];
    if (e.Stream == 0xff)
      break;
    if (e.Stream != 0 || e.UsageIndex != 0)
      continue;
    if (e.Usage == D3DDECLUSAGE_POSITION && !layout.position) {
      layout.position = true;
      layout.positionFloat3 = e.Type == D3DDECLTYPE_FLOAT3;
      layout.positionOffset = e.Offset;
    } else if (e.Usage == D3DDECLUSAGE_TEXCOORD && !layout.texcoord) {
      layout.texcoord = true;
      layout.texcoordFloat2 = e.Type == D3DDECLTYPE_FLOAT2;
      layout.texcoordOffset = e.Offset;
    }
  }
  return true;
}

float RadiusOfKind(uint8_t kind) {
  switch (ObjectKind(kind)) {
    case ObjectKind::Unit: return 260.0f;
    case ObjectKind::Building: return 900.0f;
    case ObjectKind::Destructible: return 750.0f;
    case ObjectKind::Item: return 220.0f;
    case ObjectKind::Effect: return 900.0f;
    default: return 0.0f;
  }
}

// The game object behind a world-object draw. Game.dll 1.27a keeps, per render
// group, the list of world objects it draws this frame: 24-byte elements with
// the object's sprite (the draw's world object entry) at +0x00 and its owner
// at +0x14, the list at world + (91 + group) * 4, its data at +0x0C and its
// length at +0x14 - the lists the in-process collector reads
// (war3_scene_collector.cpp). The render-group hook records the world, so
// they are read straight from the game: object identity tracking would cost a
// dense map ~190 ms a frame (2026-11-19).
//
// Measured in game on 2026-11-19: units and buildings share the CUnit vtable
// and differ by the building flag; trees and the other destructibles have the
// CDestructable vtable. Anything else stays unknown. The handle is the
// object's own, the same for all its parts.
constexpr uintptr_t kCUnitVTableRva = 0xA4A704u;
constexpr uintptr_t kCDestructableVTableRva = 0xA4FFBCu;
constexpr uintptr_t kCItemVTableRva = 0xA4A2ECu; // an item on the ground (生与死, 'I00B')
constexpr uint32_t kWorldGroupCount = 4u;
constexpr uint32_t kWorldGroupListIndex = 91u;
constexpr uint32_t kWorldGroupMaxElements = 200000u;

// The owner of another type, once per type for the log: what else the lists
// hold (items, effects), for a later rule.
void NoteOtherOwner(uint32_t vtable, uint32_t rawcode, uintptr_t base) {
  static std::array<uint32_t, 8> seen{};
  static uint32_t count = 0u;
  for (uint32_t i = 0; i < count; ++i)
    if (seen[i] == vtable)
      return;
  if (count == seen.size())
    return;
  seen[count++] = vtable;
  Logger::info(str::format("WarVK proxy mode: caster owner of another type vtable=+0x", std::hex,
                           vtable - uint32_t(base), " rawcode=0x", rawcode));
}

class OwnerTable {
public:
  // The lists hold every object of a group and change only as objects come
  // and go: the table is kept across frames, read again when an entry is
  // missing (at most every kMissFrames frames), every kRefreshFrames frames,
  // and when the world changes. An object new to the table has no class for a
  // few frames at most.
  //
  // Each object is read once, when it enters the table, with one copy of its
  // head. A defensive read per draw is a VirtualQuery, a system call: four of
  // them per draw cost LostTemple 10 ms a frame (2026-11-19).
  static constexpr uint64_t kMissFrames = 6u;
  static constexpr uint64_t kRefreshFrames = 90u;

  struct Owner {
    uint8_t kind = uint8_t(ObjectKind::Unknown);
    bool listed = false;
    uint32_t handle = 0u;
  };

  Owner find(void* entry, uint64_t frame) {
    void* world = state::RenderState::instance().getWorldPointer();
    const uint32_t key = uint32_t(reinterpret_cast<uintptr_t>(entry));
    if (m_readFrame == 0u || world != m_world || frame - m_readFrame >= kRefreshFrames)
      read(world, frame);
    const Item* item = lookup(m_items, key);
    if (item == nullptr && frame - m_readFrame >= kMissFrames) {
      read(world, frame);
      item = lookup(m_items, key);
    }
    if (item == nullptr)
      return {};
    return { item->kind, true, item->handle };
  }

private:
  struct Item {
    uint32_t entry, owner, handle;
    uint8_t kind;
  };

  static const Item* lookup(const std::vector<Item>& items, uint32_t entry) {
    const auto it = std::lower_bound(items.begin(), items.end(), entry,
        [](const Item& i, uint32_t value) { return i.entry < value; });
    return it != items.end() && it->entry == entry ? &*it : nullptr;
  }

  // What an owner is, from one copy of its head (Game.dll 1.27a, measured in
  // game on 2026-11-19): units and buildings share the CUnit vtable and differ
  // by the building flag; trees and the other destructibles have the
  // CDestructable vtable. The handle is the object's own, the same for all
  // its parts. Below 16 MiB an owner is a handle number, not an object.
  static void resolve(Item& item, uintptr_t base) {
    item.kind = uint8_t(ObjectKind::Unknown);
    item.handle = 0u;
    uint32_t head[0x60 / 4] = {};
    if (item.owner < 0x01000000u || item.owner == 0xFFFFFFFFu || base == 0u ||
        !SafeCopy(head, reinterpret_cast<const void*>(uintptr_t(item.owner)), sizeof(head)))
      return;
    const uint32_t vtable = head[CUnitOffsets::VTable / 4];
    if (vtable == base + kCUnitVTableRva) {
      item.kind = uint8_t((head[CUnitOffsets::Flags5C / 4] & UnitFlags5C::Building) != 0u
                          ? ObjectKind::Building : ObjectKind::Unit);
    } else if (vtable == base + kCDestructableVTableRva) {
      item.kind = uint8_t(ObjectKind::Destructible);
    } else if (vtable == base + kCItemVTableRva) {
      item.kind = uint8_t(ObjectKind::Item);
    } else {
      NoteOtherOwner(vtable, head[CUnitOffsets::Rawcode / 4], base);
      return;
    }
    const uint32_t id0C = head[CUnitOffsets::HashId0C / 4], id10 = head[CUnitOffsets::HashId10 / 4];
    if (id0C != 0u && id0C < 0x100000u && id0C == id10)
      item.handle = 0x100000u | id0C;
  }

  void read(void* world, uint64_t frame) {
    const auto begin = std::chrono::steady_clock::now();
    const bool sameWorld = world == m_world;
    m_world = world;
    m_readFrame = frame;
    m_previous.swap(m_items);
    if (!sameWorld)
      m_previous.clear();
    m_items.clear();
    uint32_t lists[kWorldGroupCount] = {};
    if (world != nullptr && SafeCopy(lists, static_cast<const uint8_t*>(world) + kWorldGroupListIndex * 4u,
                                     sizeof(lists))) {
      for (uint32_t list : lists) {
        uint32_t header[6] = {};
        if (list == 0u || !SafeCopy(header, reinterpret_cast<const void*>(uintptr_t(list)), sizeof(header)))
          continue;
        const uint32_t data = header[3], count = header[5];
        if (data == 0u || count == 0u || count > kWorldGroupMaxElements)
          continue;
        m_elements.resize(size_t(count) * 6u);
        if (!SafeCopy(m_elements.data(), reinterpret_cast<const void*>(uintptr_t(data)),
                      m_elements.size() * sizeof(uint32_t)))
          continue;
        for (uint32_t i = 0; i < count; ++i)
          if (m_elements[i * 6u] != 0u)
            m_items.push_back({ m_elements[i * 6u], m_elements[i * 6u + 5u], 0u, uint8_t(ObjectKind::Unknown) });
      }
      std::sort(m_items.begin(), m_items.end(), [](const Item& a, const Item& b) { return a.entry < b.entry; });
    }
    // An object already in the table keeps what it was read as: the same
    // sprite with the same owner is the same object.
    const uintptr_t base = GetGameDllBase();
    uint32_t resolved = 0u;
    for (Item& item : m_items) {
      const Item* known = lookup(m_previous, item.entry);
      if (known != nullptr && known->owner == item.owner) {
        item.kind = known->kind;
        item.handle = known->handle;
      } else {
        resolve(item, base);
        ++resolved;
      }
    }
    // A few lines for the log: how big the lists are and what a read costs.
    ++m_reads;
    if (m_logs < 6u && m_reads >= 4u && (m_reads & (m_reads - 1u)) == 0u) {
      ++m_logs;
      const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - begin).count();
      Logger::info(str::format("WarVK proxy mode: caster owner lists read=", m_reads, " frame=", frame,
                               " entries=", m_items.size(), " resolved=", resolved, " us=", us));
    }
  }

  void* m_world = nullptr;
  uint64_t m_readFrame = 0u;
  uint64_t m_reads = 0u;
  uint32_t m_logs = 0u;
  std::vector<uint32_t> m_elements;
  std::vector<Item> m_items, m_previous;
};

OwnerTable s_owners;

uint8_t KindOfWidget(void* worldObjectEntry, uint32_t& handle) {
  const OwnerTable::Owner owner = s_owners.find(worldObjectEntry, c.frame);
  handle = owner.handle;
  // A few lines for the log: what the lists said about the casters.
  static std::array<uint32_t, 5> counts{};
  static uint32_t total = 0u, logs = 0u;
  ++counts[owner.kind == uint8_t(ObjectKind::Unit) ? 0 : owner.kind == uint8_t(ObjectKind::Building) ? 1
           : owner.kind == uint8_t(ObjectKind::Destructible) ? 2 : owner.listed ? 3 : 4];
  if (logs < 4u && ++total == (4096u << logs)) {
    ++logs;
    Logger::info(str::format("WarVK proxy mode: caster owners unit=", counts[0], " building=", counts[1],
                             " destructible=", counts[2], " other=", counts[3], " unlisted=", counts[4]));
  }
  return owner.kind;
}

// The game's water (Game.dll 1.27a, measured in the sandbox on 2026-11-19 by
// the probe below): drawn in the world scene outside any stage the hooks name,
// as flat quads in world space (identity world matrix) from a dynamic buffer,
// FVF XYZ|NORMAL|DIFFUSE|TEX1, an indexed triangle list of two triangles per
// four vertices, alpha blended over the ground with the depth test and no
// depth write. Hiding exactly these draws hid the water and nothing else.
bool IsWaterDraw(IDirect3DDevice9* device, const WarVKD3D9DrawFacts& facts) {
  if (facts.indexed == 0u || facts.primitiveType != D3DPT_TRIANGLELIST || facts.primitiveCount == 0u ||
      facts.primitiveCount > 0x00ffffffu || facts.numVertices != 2u * facts.primitiveCount)
    return false;
  if (RenderStateOf(device, D3DRS_ZENABLE) == FALSE || RenderStateOf(device, D3DRS_ZWRITEENABLE) != FALSE ||
      RenderStateOf(device, D3DRS_ALPHABLENDENABLE) == FALSE ||
      RenderStateOf(device, D3DRS_SRCBLEND) != D3DBLEND_SRCALPHA ||
      RenderStateOf(device, D3DRS_DESTBLEND) != D3DBLEND_INVSRCALPHA)
    return false;
  DWORD fvf = 0;
  if (FAILED(device->GetFVF(&fvf)) || fvf != (D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1))
    return false;
  IDirect3DVertexShader9* shader = nullptr;
  const bool bound = SUCCEEDED(device->GetVertexShader(&shader)) && shader != nullptr;
  if (shader != nullptr)
    shader->Release();
  if (bound)
    return false;
  D3DMATRIX world = {};
  if (FAILED(device->GetTransform(D3DTS_WORLD, &world)))
    return false;
  static const D3DMATRIX identity = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
  return std::memcmp(&world, &identity, sizeof(world)) == 0;
}

// The water's record: no class, no bounds, never a caster. The host replays
// it for the depth of the water's surface (War3FrameScene::waterSurfaces).
HRESULT SendWater(IDirect3DDevice9* device, const WarVKD3D9DrawFacts& facts) {
  Layout layout;
  IDirect3DVertexBuffer9* vertices = nullptr;
  IDirect3DIndexBuffer9* indices = nullptr;
  UINT streamOffset = 0, stride = 0;
  bool vertexDynamic = false, indexDynamic = false;
  bool replayable = ReadLayout(device, layout) && layout.position && layout.positionFloat3 &&
                    SUCCEEDED(device->GetStreamSource(0, &vertices, &streamOffset, &stride)) &&
                    vertices != nullptr && stride >= layout.positionOffset + 12u &&
                    SUCCEEDED(device->GetIndices(&indices)) && indices != nullptr;
  if (replayable) {
    D3DVERTEXBUFFER_DESC vertexDesc = {};
    D3DINDEXBUFFER_DESC indexDesc = {};
    replayable = SUCCEEDED(vertices->GetDesc(&vertexDesc)) && SUCCEEDED(indices->GetDesc(&indexDesc)) &&
                 indexDesc.Format == D3DFMT_INDEX16;
    vertexDynamic = (vertexDesc.Usage & D3DUSAGE_DYNAMIC) != 0u;
    indexDynamic = (indexDesc.Usage & D3DUSAGE_DYNAMIC) != 0u;
  }
  if (vertices != nullptr)
    vertices->Release();
  if (indices != nullptr)
    indices->Release();
  if (!replayable) {
    Skip(CasterSkip::NotReplayable);
    return S_FALSE;
  }
  scene::ShadowCaster record;
  D3DMATRIX world = {};
  device->GetTransform(D3DTS_WORLD, &world);
  std::memcpy(record.world.data(), &world, sizeof(world));
  const auto sizes = scene::CasterCopySizes::forDraw(vertexDynamic ? facts.numVertices : 0u, false,
      indexDynamic ? facts.primitiveCount * 3u : 0u, 2u);
  if (!c.pool.wants(sizes, scene::CasterFrameRecordLimit)) {
    Skip(CasterSkip::Budget);
    if (c.pool.omitted == 1u)
      ++c.status.omittedFrames;
    return S_FALSE;
  }
  record.category = uint32_t(Category::Unknown);
  record.batchTag = -1;
  record.stage = -1;
  record.flags = scene::ShadowCasterFlag::DepthTest | scene::ShadowCasterFlag::AlphaBlend;
  record.rawcode = scene::WaterSurfaceRawcode;
  record.part = scene::WaterSurfacePart;
  const HRESULT hr = AnnotateNextDraw(scene::ShadowCasterSchema, scene::EncodeShadowCaster(record).data(),
                                      scene::ShadowCasterBytes);
  if (FAILED(hr))
    return hr;
  c.frameCounts.admittedBytes = uint32_t(std::min<uint64_t>(c.pool.demand.frameBytes, UINT32_MAX));
  // A record of the frame: the ShadowFrame record's count includes it.
  ++c.sent;
  ++c.status.sent;
  ++c.status.water;
  ++c.frameCounts.sent;
  c.status.dynamicBytes += (vertexDynamic ? uint64_t(facts.numVertices) * stride : 0u) + sizes.index;
  static uint32_t s_logged = 0u;
  if (s_logged < 2u) {
    ++s_logged;
    Logger::info(str::format("WarVK proxy mode: water record vertices=", facts.numVertices,
                             " triangles=", facts.primitiveCount, " stride=", stride,
                             " dynamic=", vertexDynamic ? 1 : 0, indexDynamic ? 1 : 0));
  }
  return S_OK;
}

// Sandbox probe (WARVK_D3D9_WATER_PROBE): how the game draws its water.
bool WaterProbeOn() {
  static const bool on = GetEnvironmentVariableW(L"WARVK_D3D9_WATER_PROBE", nullptr, 0) > 0;
  return on;
}

void ProbeDraw(IDirect3DDevice9* device, const WarVKD3D9DrawFacts& facts, int stage, Category category,
               War3BatchTag tag, War3BatchTag execTag) {
  static std::array<uint32_t, 34> histogram{};
  static uint64_t histogramFrame = 0u;
  static uint32_t logged = 0u, histograms = 0u;
  if (histogramFrame != c.frame) {
    if (histogramFrame != 0u && histograms < 6u && (c.frame == 300u || c.frame == 900u || c.frame == 1500u)) {
      ++histograms;
      std::string line;
      for (size_t i = 0; i < histogram.size(); ++i)
        if (histogram[i] != 0u)
          line += str::format(" s", int(i) - 1, "=", histogram[i]);
      Logger::info(str::format("WarVK water probe: frame=", c.frame, " draws by stage:", line));
    }
    histogram.fill(0u);
    histogramFrame = c.frame;
  }
  ++histogram[size_t(std::clamp(stage + 1, 0, 33))];
  const bool water = stage == 14 || stage == 2 || stage == 6 || stage == -1 || stage == 19 ||
                     tag == War3BatchTag::Water || execTag == War3BatchTag::Water;
  if (!water || logged >= 60u || c.frame < 200u)
    return;
  // One line per kind of draw: stage, layout, blend and depth state.
  static std::array<uint64_t, 64> seen{};
  static uint32_t seenCount = 0u;
  DWORD fvfKey = 0;
  device->GetFVF(&fvfKey);
  const uint64_t key = (uint64_t(uint32_t(stage + 1) & 0xffu) << 56) ^ (uint64_t(fvfKey) << 24) ^
      (uint64_t(RenderStateOf(device, D3DRS_ALPHABLENDENABLE)) << 20) ^ (uint64_t(RenderStateOf(device, D3DRS_SRCBLEND)) << 14) ^
      (uint64_t(RenderStateOf(device, D3DRS_DESTBLEND)) << 8) ^ (uint64_t(RenderStateOf(device, D3DRS_ZWRITEENABLE)) << 4) ^
      uint64_t(facts.primitiveType) ^ (uint64_t(facts.indexed) << 3) ^ (uint64_t(uint32_t(category)) << 40);
  for (uint32_t i = 0; i < seenCount; ++i)
    if (seen[i] == key)
      return;
  if (seenCount < seen.size())
    seen[seenCount++] = key;
  ++logged;
  std::string decl;
  IDirect3DVertexDeclaration9* declaration = nullptr;
  if (SUCCEEDED(device->GetVertexDeclaration(&declaration)) && declaration != nullptr) {
    D3DVERTEXELEMENT9 elements[MAXD3DDECLLENGTH + 1];
    UINT count = MAXD3DDECLLENGTH + 1;
    if (SUCCEEDED(declaration->GetDeclaration(elements, &count)))
      for (UINT i = 0; i < count && elements[i].Stream != 0xff; ++i)
        decl += str::format(" ", uint32_t(elements[i].Stream), ":", uint32_t(elements[i].Usage), "/",
                            uint32_t(elements[i].Type), "@", elements[i].Offset);
    declaration->Release();
  }
  DWORD fvf = 0;
  device->GetFVF(&fvf);
  IDirect3DVertexBuffer9* vertices = nullptr;
  UINT offset = 0, stride = 0;
  D3DVERTEXBUFFER_DESC vdesc = {};
  if (SUCCEEDED(device->GetStreamSource(0, &vertices, &offset, &stride)) && vertices != nullptr) {
    vertices->GetDesc(&vdesc);
    vertices->Release();
  }
  IDirect3DIndexBuffer9* indices = nullptr;
  D3DINDEXBUFFER_DESC idesc = {};
  if (SUCCEEDED(device->GetIndices(&indices)) && indices != nullptr) {
    indices->GetDesc(&idesc);
    indices->Release();
  }
  IDirect3DVertexShader9* shader = nullptr;
  const bool vs = SUCCEEDED(device->GetVertexShader(&shader)) && shader != nullptr;
  if (shader != nullptr)
    shader->Release();
  D3DMATRIX world = {};
  device->GetTransform(D3DTS_WORLD, &world);
  IDirect3DBaseTexture9* texture = nullptr;
  const bool tex0 = SUCCEEDED(device->GetTexture(0, &texture)) && texture != nullptr;
  if (texture != nullptr)
    texture->Release();
  Logger::info(str::format("WarVK water probe: frame=", c.frame, " stage=", stage, " cat=", uint32_t(category),
      " tag=", int(tag), "/", int(execTag), " layer=", int(War3RenderState::CurrentLayer()),
      " indexed=", facts.indexed, " prim=", facts.primitiveType, " tris=", facts.primitiveCount,
      " verts=", facts.numVertices, " fvf=0x", std::hex, fvf, std::dec, " decl=[", decl, "]",
      " stride=", stride, " off=", offset, " vbUsage=0x", std::hex, vdesc.Usage, " vbPool=", vdesc.Pool,
      " ibFmt=", idesc.Format, std::dec, " vs=", vs ? 1 : 0, " tex0=", tex0 ? 1 : 0,
      " z=", RenderStateOf(device, D3DRS_ZENABLE), "/", RenderStateOf(device, D3DRS_ZWRITEENABLE),
      " blend=", RenderStateOf(device, D3DRS_ALPHABLENDENABLE), ":", RenderStateOf(device, D3DRS_SRCBLEND),
      ">", RenderStateOf(device, D3DRS_DESTBLEND), " atest=", RenderStateOf(device, D3DRS_ALPHATESTENABLE),
      " light=", RenderStateOf(device, D3DRS_LIGHTING), " cull=", RenderStateOf(device, D3DRS_CULLMODE),
      " world=[", world._11, ",", world._22, ",", world._33, " t=", world._41, ",", world._42, ",", world._43, "]"));
}


// Sandbox probe (WARVK_D3D9_UNIT_LIGHT_PROBE=1): how the game lights the world
// objects' draws, one line per texture and state, and what the world lighting
// sends instead. Proxy mode knows no unit type, so the map shows one unit.
struct UnitLightProbe {
  bool on = false;
  War3DirectionalLightSettings sun{};
  decltype(War3RenderSettings::ambient) ambient{};
};
UnitLightProbe& LightProbe() {
  static UnitLightProbe probe = [] {
    UnitLightProbe p;
    p.on = GetEnvironmentVariableW(L"WARVK_D3D9_UNIT_LIGHT_PROBE", nullptr, 0) > 0;
    return p;
  }();
  return probe;
}

void ProbeUnitLight(IDirect3DDevice9* device, const WarVKD3D9DrawFacts& facts, int stage, Category category,
                    War3BatchTag tag) {
  static uint32_t logged = 0u;
  static std::array<uint64_t, 64> seen{};
  static uint32_t seenCount = 0u;
  if (logged >= 64u || c.frame < 600u)
    return;
  IDirect3DBaseTexture9* texture = nullptr;
  device->GetTexture(0, &texture);
  if (texture != nullptr)
    texture->Release();
  const DWORD lighting = RenderStateOf(device, D3DRS_LIGHTING);
  const DWORD blend = RenderStateOf(device, D3DRS_ALPHABLENDENABLE);
  const DWORD src = RenderStateOf(device, D3DRS_SRCBLEND), dst = RenderStateOf(device, D3DRS_DESTBLEND);
  const uint64_t key = uint64_t(reinterpret_cast<uintptr_t>(texture)) ^ (uint64_t(lighting) << 40) ^
                       (uint64_t(blend) << 44) ^ (uint64_t(src) << 48) ^ (uint64_t(dst) << 56) ^
                       (uint64_t(uint32_t(stage + 1) & 0xffu) << 32);
  for (uint32_t i = 0; i < seenCount; ++i)
    if (seen[i] == key)
      return;
  if (seenCount < seen.size())
    seen[seenCount++] = key;
  ++logged;
  D3DMATERIAL9 m = {};
  device->GetMaterial(&m);
  D3DLIGHT9 l = {};
  device->GetLight(0, &l);
  BOOL l0 = FALSE;
  device->GetLightEnable(0, &l0);
  DWORD op0 = 0, a01 = 0, a02 = 0, op1 = 0, a11 = 0, a12 = 0, aop0 = 0;
  device->GetTextureStageState(0, D3DTSS_COLOROP, &op0);
  device->GetTextureStageState(0, D3DTSS_COLORARG1, &a01);
  device->GetTextureStageState(0, D3DTSS_COLORARG2, &a02);
  device->GetTextureStageState(0, D3DTSS_ALPHAOP, &aop0);
  device->GetTextureStageState(1, D3DTSS_COLOROP, &op1);
  device->GetTextureStageState(1, D3DTSS_COLORARG1, &a11);
  device->GetTextureStageState(1, D3DTSS_COLORARG2, &a12);
  const auto c4 = [](const D3DCOLORVALUE& v) {
    return str::format(v.r, ",", v.g, ",", v.b, ",", v.a);
  };
  const auto& p = LightProbe();
  const float sr = p.sun.color.x * p.sun.intensity, sg = p.sun.color.y * p.sun.intensity,
              sb = p.sun.color.z * p.sun.intensity;
  Logger::info(str::format(
      "WarVK unit light probe: frame=", c.frame, " tag=", int(tag), " stage=", stage,
      " cat=", uint32_t(category), " tris=", facts.primitiveCount, " tex=", texture != nullptr ? 1 : 0,
      " light=", lighting, " l0=", l0, " blend=", blend, ":", src, ">", dst,
      " atest=", RenderStateOf(device, D3DRS_ALPHATESTENABLE), " zw=", RenderStateOf(device, D3DRS_ZWRITEENABLE),
      " colorVertex=", RenderStateOf(device, D3DRS_COLORVERTEX),
      " src d/a/e/s=", RenderStateOf(device, D3DRS_DIFFUSEMATERIALSOURCE), "/",
      RenderStateOf(device, D3DRS_AMBIENTMATERIALSOURCE), "/", RenderStateOf(device, D3DRS_EMISSIVEMATERIALSOURCE),
      "/", RenderStateOf(device, D3DRS_SPECULARMATERIALSOURCE),
      " specOn=", RenderStateOf(device, D3DRS_SPECULARENABLE), " ambient=0x", std::hex,
      RenderStateOf(device, D3DRS_AMBIENT), " tfactor=0x", RenderStateOf(device, D3DRS_TEXTUREFACTOR), std::dec,
      " tss0=", op0, "(", a01, ",", a02, ") aop0=", aop0, " tss1=", op1, "(", a11, ",", a12, ")",
      " mat D=", c4(m.Diffuse), " A=", c4(m.Ambient), " E=", c4(m.Emissive), " S=", c4(m.Specular), " P=", m.Power,
      " game L0 type=", uint32_t(l.Type), " D=", c4(l.Diffuse), " A=", c4(l.Ambient), " S=", c4(l.Specular),
      " ours sun=", int(p.sun.enabled), " D=", sr, ",", sg, ",", sb, " A=", sr * 0.2f, ",", sg * 0.2f, ",", sb * 0.2f,
      " ambOverride=", int(p.ambient.overrideEnabled), " ", p.ambient.color.x, ",", p.ambient.color.y, ",",
      p.ambient.color.z));
}

} // namespace

void CastersBeginFrame(const War3RenderSettings& settings) {
  c.sent = c.dynamicCasters = 0u;
  c.signature = 0u;
  c.pool = {};
  c.classes = settings.casterClasses;
  c.objectClasses = false;
  for (const auto& cls : c.classes)
    c.objectClasses |= !cls.castShadow || cls.maxShadowLength > 0.0f;
  c.frameCounts = CasterFrameCounts();
  ++c.frame;
  if (LightProbe().on) {
    LightProbe().sun = settings.sun;
    LightProbe().ambient = settings.ambient;
  }
  c.wanted = false;
  c.outlineWanted = false;
  if constexpr (!internal::kWar3RenderModuleTakeoverEnabled)
    return;

  // War3RenderPipeline::OnFrameStart, the part that decides WantsShadowCapture.
  using runtime::IsWar3RuntimeModuleEnabled;
  using runtime::War3RuntimeModule;
  if (!IsWar3RuntimeModuleEnabled(War3RuntimeModule::ShadowCapture))
    return;
  const bool moduleShadowMap = IsWar3RuntimeModuleEnabled(War3RuntimeModule::ShadowMap);
  const bool moduleShadowReceiver = IsWar3RuntimeModuleEnabled(War3RuntimeModule::ShadowReceiver);
  const bool disableWar3Shadow = internal::kNativeShadowDisableWar3ShadowReceiverWhenMode1 &&
                                 War3RenderState::GetNativeShadowMode() >= 1u;
  const bool outlineHandles = War3RenderState::HasOutlineHandles();
  const bool wantsOutline = moduleShadowReceiver && settings.occludedOutline.enabled && outlineHandles &&
                            !(disableWar3Shadow && internal::kNativeShadowDisableOutlineWhenMode1);
  const bool wantsShadows = moduleShadowMap && moduleShadowReceiver && settings.shadows.enabled &&
                            !disableWar3Shadow;
  const bool wantsPointShadow =
      moduleShadowReceiver && moduleShadowMap && settings.shadows.pointLightsEnabled &&
      settings.shadows.pointShadowEnabled && settings.shadows.pointShadowMaxLights > 0 &&
      War3LightManager::Instance().HasActiveLights();
  if (!wantsShadows && !wantsOutline && !wantsPointShadow)
    return;
  // The capture's own gate on the settings.
  if (!settings.shadows.enabled && !(settings.occludedOutline.enabled && outlineHandles))
    return;
  if (internal::kNativeShadowDisableShadowCaptureWhenMode1 && War3RenderState::GetNativeShadowMode() >= 1u)
    return;
  static const bool s_disabled = [] {
    const std::string value = env::getEnvVar("DXVK_WAR3_DISABLE_SHADOW_CAPTURE");
    const bool disabled = !value.empty() && value != "0";
    if (disabled)
      Logger::info("WarVK proxy mode: shadow casters disabled by DXVK_WAR3_DISABLE_SHADOW_CAPTURE");
    return disabled;
  }();
  c.wanted = !s_disabled;
  c.outlineWanted = wantsOutline;
}

bool CastersWanted() { return c.wanted; }

HRESULT CastersDraw(IDirect3DDevice9* device, const WarVKD3D9DrawFacts& facts) {
  if (!c.wanted)
    return S_FALSE;
  // Outside a map the main menu still draws models; nothing consumes them.
  if (!g_war3_runtime_activated.load(std::memory_order_relaxed))
    return S_FALSE;
  if (War3RenderState::CurrentLayer() == War3RenderLayer::UI)
    return S_FALSE;

  const int stage = War3RenderState::GetStage();
  const Category category = War3RenderState::GetStageCategory();
  const War3BatchTag tag = War3RenderState::GetCurrentBatchTag();
  const War3BatchTag execTag = War3RenderState::GetTlsBatchTag();
  const War3TlsShadowSemanticState& semantic = War3RenderState::GetTlsShadowSemanticState();
  if (WaterProbeOn())
    ProbeDraw(device, facts, stage, category, tag, execTag);
  // The water: its own record, before any gate of the casters.
  // Gated on the native CTerrain::SubmitWaterDraw scope (0x737960) so
  // alpha-blended particle emitters (CParticleEmitter2, also flushed with
  // stage=-1, identity world, XYZ|NORMAL|DIFFUSE|TEX1, SRCALPHA/INVSRCALPHA,
  // ZWRITE=0 and 2 triangles per 4 vertices) are never misclassified as water.
  if (War3RenderState::IsWaterRendering() && IsWaterDraw(device, facts))
    return SendWater(device, facts);
  if (category == Category::WorldObject && LightProbe().on)
    ProbeUnitLight(device, facts, stage, category, tag);

  const render::ShadowProducerPolicyContext policy = { stage, tag, execTag, semantic.tag, false };
  if (!render::ShadowProducerPolicyAllows(render::ShadowProducerKind::ImmediateLegacyCapture, policy)) {
    Skip(CasterSkip::Policy);
    return S_FALSE;
  }
  if (category == Category::PostProcess || category == Category::Skybox || category == Category::UI ||
      category == Category::Effect)
    return S_FALSE;

  // ---- what kind of caster ----------------------------------------------------
  const bool terrainS1 = category == Category::Terrain && stage == 1;
  const bool terrainDoodad = category == Category::Terrain && stage == 10;
  const bool terrainTile = War3RenderState::IsTerrainRendering();
  const bool terrainCaster = terrainS1 || terrainDoodad || terrainTile;
  const bool stage10Doodad = stage == 10 && (category == Category::Terrain || category == Category::WorldObject);
  const bool stage13WorldObject = stage == 13 && category == Category::WorldObject;
  if (terrainDoodad && !internal::kShadowRenderDecorations) {
    Skip(CasterSkip::NotCaster);
    return S_FALSE;
  }

  const render::RenderObjectInfo* currentObject = terrainCaster ? nullptr : render::GetCurrentBatchObject();
  bool objectCaster = false;
  if (!terrainCaster) {
    const bool byTag = tag == War3BatchTag::WorldObjects || tag == War3BatchTag::SelectionOverlay ||
                       tag == War3BatchTag::Decorations;
    bool byStage = false;
    if (category == Category::WorldObject && (stage == 7 || stage == 10 || stage == 11 || stage == 13)) {
      const auto dispatch = render::GetCurrentDrawDispatchContext();
      byStage = dispatch.valid && dispatch.renderablePart != nullptr &&
                (dispatch.domain == render::CurrentDrawDispatchDomain::Common ||
                 dispatch.domain == render::CurrentDrawDispatchDomain::Special ||
                 dispatch.domain == render::CurrentDrawDispatchDomain::TransparentType0);
    }
    const bool byObject = currentObject != nullptr && currentObject->kind != ObjectKind::Unknown;
    objectCaster = byTag || byStage || byObject;
  }
  if (!terrainCaster && !objectCaster) {
    Skip(CasterSkip::NotCaster);
    return S_FALSE;
  }
  ++c.status.considered;
  ++c.frameCounts.considered;

  // ---- identity ---------------------------------------------------------------
  // The batch's handle is what the unit outline matches: the dispatcher's
  // first, then the batch object's, then the semantic bridge's; one format.
  const render::RenderObjectInfo* identityObject = semantic.object != nullptr ? semantic.object : currentObject;
  uint32_t batchHandle = War3RenderState::GetTlsBatchHandle();
  if (batchHandle == 0u && currentObject != nullptr)
    batchHandle = currentObject->jHandle;
  if (batchHandle == 0u)
    batchHandle = semantic.jHandle;
  if (batchHandle != 0u) {
    const uint32_t handleId = batchHandle & 0x0FFFFFu;
    batchHandle = handleId ? (0x100000u | handleId) : 0u;
  }
  const render::RenderObjectInfo* registered = currentObject;
  if ((registered == nullptr || registered->rawcode == 0u) && batchHandle != 0u) {
    if (const auto* found = render::RenderObjectRegistry::instance().findByHandle(batchHandle))
      registered = found;
  }
  const uint32_t rawcode = semantic.rawcode != 0u ? semantic.rawcode
                         : registered != nullptr ? registered->rawcode : 0u;

  // A pathing or line-of-sight blocker is a hidden model: it casts nothing.
  // The S1 tiles are never one.
  if (internal::kPathBlockerHideEnabled && !terrainS1) {
    bool blocker = semantic.pathBlocker || IsBlocker(rawcode) ||
                   (registered != nullptr && IsBlocker(registered->rawcode));
    if (!blocker && rawcode == 0u) {
      blocker = WidgetIsBlocker(semantic.worldObjectEntry) ||
                (identityObject != nullptr && (WidgetIsBlocker(identityObject->worldObjectEntry) ||
                                               WidgetIsBlocker(identityObject->unitPtr)));
    }
    if (blocker) {
      Skip(CasterSkip::PathBlocker);
      return S_FALSE;
    }
  }

  // ---- render state -----------------------------------------------------------
  // The front end's device always has its automatic depth buffer bound (the
  // game never replaces it), so the depth test is the render state alone.
  if (RenderStateOf(device, D3DRS_ZENABLE) == FALSE) {
    Skip(CasterSkip::NoDepth);
    return S_FALSE;
  }
  {
    IDirect3DVertexShader9* shader = nullptr;
    const bool bound = SUCCEEDED(device->GetVertexShader(&shader)) && shader != nullptr;
    if (shader != nullptr)
      shader->Release();
    const bool allowed = terrainCaster ||
                         (objectCaster && (tag == War3BatchTag::Decorations || stage10Doodad || stage13WorldObject));
    if (bound && !allowed) {
      Skip(CasterSkip::VertexShader);
      return S_FALSE;
    }
  }
  const bool alphaTest = RenderStateOf(device, D3DRS_ALPHATESTENABLE) != FALSE;
  const bool depthWrite = RenderStateOf(device, D3DRS_ZWRITEENABLE) != FALSE;
  const bool alphaBlend = RenderStateOf(device, D3DRS_ALPHABLENDENABLE) != FALSE;
  // A draw that neither writes depth nor is a cutout is an overlay.
  if ((terrainS1 || terrainDoodad) ? (!depthWrite && !alphaTest) : (terrainTile && !depthWrite)) {
    Skip(CasterSkip::Overlay);
    return S_FALSE;
  }
  if (alphaBlend) {
    const DWORD source = RenderStateOf(device, D3DRS_SRCBLEND);
    const DWORD destination = RenderStateOf(device, D3DRS_DESTBLEND);
    if (RenderStateOf(device, D3DRS_BLENDOP) == D3DBLENDOP_ADD && destination == D3DBLEND_ONE &&
        (source == D3DBLEND_ONE || source == D3DBLEND_SRCALPHA)) {
      Skip(CasterSkip::Additive); // light, not a surface
      return S_FALSE;
    }
    if (!alphaTest) {
      Skip(CasterSkip::BlendNoAlphaTest);
      return S_FALSE;
    }
  }

  // ---- what the host can replay -------------------------------------------------
  Layout layout;
  IDirect3DVertexBuffer9* vertices = nullptr;
  IDirect3DIndexBuffer9* indices = nullptr;
  UINT streamOffset = 0, stride = 0;
  bool vertexDynamic = false, indexDynamic = false;
  bool replayable = facts.indexed != 0u && facts.primitiveType == D3DPT_TRIANGLELIST &&
                    facts.primitiveCount != 0u && facts.primitiveCount <= 0x00ffffffu &&
                    facts.numVertices != 0u && ReadLayout(device, layout) && layout.position &&
                    layout.positionFloat3;
  if (replayable) {
    replayable = SUCCEEDED(device->GetStreamSource(0, &vertices, &streamOffset, &stride)) &&
                 vertices != nullptr && stride >= layout.positionOffset + 12u &&
                 SUCCEEDED(device->GetIndices(&indices)) && indices != nullptr;
    if (replayable) {
      D3DVERTEXBUFFER_DESC vertexDesc = {};
      D3DINDEXBUFFER_DESC indexDesc = {};
      replayable = SUCCEEDED(vertices->GetDesc(&vertexDesc)) && SUCCEEDED(indices->GetDesc(&indexDesc)) &&
                   indexDesc.Format == D3DFMT_INDEX16;
      vertexDynamic = (vertexDesc.Usage & D3DUSAGE_DYNAMIC) != 0u;
      indexDynamic = (indexDesc.Usage & D3DUSAGE_DYNAMIC) != 0u;
    }
    if (vertices != nullptr)
      vertices->Release();
    if (indices != nullptr)
      indices->Release();
  }
  if (!replayable) {
    Skip(CasterSkip::NotReplayable);
    static uint32_t s_logged = 0;
    if (s_logged < 16u) {
      ++s_logged;
      Logger::info(str::format("WarVK proxy mode: caster not replayable stage=", stage,
                               " category=", uint32_t(category), " indexed=", facts.indexed,
                               " primitive=", facts.primitiveType, " triangles=", facts.primitiveCount,
                               " vertices=", facts.numVertices, " position=", layout.position ? 1 : 0,
                               layout.positionFloat3 ? 1 : 0, "@", layout.positionOffset,
                               " stride=", stride, " vb=", vertices != nullptr ? 1 : 0,
                               " ib=", indices != nullptr ? 1 : 0));
    }
    return S_FALSE;
  }
  {
    const DWORD blend = RenderStateOf(device, D3DRS_VERTEXBLEND);
    const bool indexed = RenderStateOf(device, D3DRS_INDEXEDVERTEXBLENDENABLE) != FALSE;
    if ((blend != D3DVBF_DISABLE || indexed) && !(blend == D3DVBF_0WEIGHTS && !indexed)) {
      Skip(CasterSkip::VertexBlend);
      return S_FALSE;
    }
  }
  // An alpha-tested draw casts through its texture or not at all: it never
  // becomes an opaque card.
  if (alphaTest) {
    IDirect3DBaseTexture9* texture = nullptr;
    const bool bound = SUCCEEDED(device->GetTexture(0, &texture)) && texture != nullptr;
    if (texture != nullptr)
      texture->Release();
    if (!bound || !layout.texcoord || !layout.texcoordFloat2 || stride < layout.texcoordOffset + 8u) {
      Skip(CasterSkip::AlphaPayload);
      return S_FALSE;
    }
  }

  // ---- object class -------------------------------------------------------------
  uint8_t kind = uint8_t(currentObject != nullptr && currentObject->kind != ObjectKind::Unknown
                             ? currentObject->kind : semantic.objectKind);
  // A unit is one only with evidence of a unit behind the draw.
  const bool unitLane = semantic.tag == War3BatchTag::WorldObjects ||
                        semantic.tag == War3BatchTag::SelectionOverlay || semantic.stage == 11 ||
                        tag == War3BatchTag::WorldObjects || tag == War3BatchTag::SelectionOverlay ||
                        (stage == 11 && category == Category::WorldObject);
  bool unitProven = false;
  if (!terrainCaster) {
    unitProven = unitLane && (batchHandle != 0u || rawcode != 0u || semantic.worldObjectEntry != nullptr ||
                              semantic.sceneNode != nullptr);
    if (!unitProven && identityObject != nullptr) {
      unitProven = identityObject->unitPtr != nullptr || identityObject->agentPtr != nullptr ||
                   (identityObject->kind == ObjectKind::Unit &&
                    (identityObject->groupIdx == 0 ||
                     (unitLane && (identityObject->jHandle != 0u || identityObject->rawcode != 0u))));
    }
  }
  if (kind == uint8_t(ObjectKind::Unit) && !unitProven)
    kind = uint8_t(ObjectKind::Unknown);
  // In proxy mode the draw itself says nothing about its object. While the
  // player has set a class apart, the game's own world-object lists name the
  // object behind the draw, and the object says what it is.
  uint32_t widgetHandle = 0u;
  if (c.objectClasses && !terrainCaster && semantic.worldObjectEntry != nullptr) {
    const uint8_t widgetKind = KindOfWidget(semantic.worldObjectEntry, widgetHandle);
    if (widgetKind != uint8_t(ObjectKind::Unknown))
      kind = widgetKind;
  }

  const auto cls = scene::ClassifyCaster(uint32_t(category), stage, kind, unitProven);
  // A class switched off costs nothing, except for the object the occluded
  // outline selects: the outline is drawn from these same records, and the
  // directional pass drops the draw by its class.
  if (cls != scene::CasterClass::Terrain && !c.classes[size_t(cls)].castShadow &&
      !(c.outlineWanted && batchHandle != 0u && War3RenderState::IsOutlineHandle(batchHandle)))
    return S_FALSE;

  // The hidden marker of a blocker whose identity is not known: a few
  // vertices, opaque, in the unit or decoration lanes.
  if (internal::kPathBlockerHideEnabled && internal::kPathBlockerBelowGroundFlatMarkerGateEnabled &&
      internal::kPathBlockerBelowGroundFlatMarkerUnreadableFallbackEnabled && !terrainS1 &&
      rawcode == 0u && !alphaTest && !alphaBlend &&
      facts.numVertices >= 3u && facts.numVertices <= internal::kPathBlockerBelowGroundFlatMarkerMaxVertices &&
      facts.primitiveCount * 3u <= internal::kPathBlockerBelowGroundFlatMarkerMaxIndices) {
    const War3BatchTag batchTag = execTag != War3BatchTag::Unknown ? execTag : tag;
    const bool lane = (stage == 11 && (category == Category::WorldObject || batchTag == War3BatchTag::WorldObjects)) ||
                      category == Category::Terrain || batchTag == War3BatchTag::Terrain ||
                      batchTag == War3BatchTag::Decorations;
    const bool unitBehind = identityObject != nullptr &&
                            (identityObject->unitPtr != nullptr || identityObject->agentPtr != nullptr);
    if (lane && kind != uint8_t(ObjectKind::Building) && kind != uint8_t(ObjectKind::Effect) && !unitBehind) {
      Skip(CasterSkip::Marker);
      return S_FALSE;
    }
  }

  // ---- the record ---------------------------------------------------------------
  D3DMATRIX world = {};
  if (FAILED(device->GetTransform(D3DTS_WORLD, &world))) {
    Skip(CasterSkip::NotReplayable);
    return S_FALSE;
  }
  scene::ShadowCaster record;
  std::memcpy(record.world.data(), &world, sizeof(world));
  for (float value : record.world) {
    if (!std::isfinite(value)) {
      Skip(CasterSkip::NotReplayable);
      return S_FALSE;
    }
  }
  const auto sizes = scene::CasterCopySizes::forDraw(vertexDynamic ? facts.numVertices : 0u, alphaTest,
      indexDynamic ? facts.primitiveCount * 3u : 0u, 2u);
  if (!c.pool.wants(sizes, scene::CasterFrameRecordLimit)) {
    Skip(CasterSkip::Budget);
    if (c.pool.omitted == 1u)
      ++c.status.omittedFrames;
    return S_FALSE;
  }

  record.category = uint32_t(category);
  record.batchTag = int32_t(execTag != War3BatchTag::Unknown ? execTag : tag);
  record.stage = stage < -1 || stage > 255 ? -1 : stage;
  record.batchHandle = batchHandle;
  record.rawcode = rawcode;
  record.objectHandle = semantic.jHandle != 0u ? semantic.jHandle
                      : batchHandle != 0u ? batchHandle : widgetHandle;
  record.objectKind = kind;
  record.flags = scene::ShadowCasterFlag::DepthTest |
                 (alphaBlend ? scene::ShadowCasterFlag::AlphaBlend : 0u) |
                 (depthWrite ? scene::ShadowCasterFlag::DepthWrite : 0u) |
                 (unitProven ? scene::ShadowCasterFlag::UnitIdentityProven : 0u);
  // Bounds as the capture gives them: a sphere of the object's class around
  // the origin of its world matrix. Terrain, and an object whose vertices are
  // already in world space (an identity matrix says nothing about where it
  // is), have none: they are never culled.
  const bool placed = world._41 != 0.0f || world._42 != 0.0f || world._43 != 0.0f;
  if (!terrainCaster && placed && (category == Category::WorldObject || category == Category::Effect)) {
    float radius = RadiusOfKind(kind);
    if (radius <= 0.0f && unitProven)
      radius = 260.0f;
    if (radius > 0.0f) {
      const auto axis = [](float x, float y, float z) { return std::sqrt(x * x + y * y + z * z); };
      const float scale = std::max(1.0f, std::max(axis(world._11, world._12, world._13),
                                   std::max(axis(world._21, world._22, world._23),
                                            axis(world._31, world._32, world._33))));
      if (std::isfinite(radius * scale)) {
        record.boundsCenter = { world._41, world._42, world._43 };
        record.boundsRadius = radius * scale;
      }
    }
  }

  HRESULT hr = AnnotateNextDraw(scene::ShadowCasterSchema, scene::EncodeShadowCaster(record).data(),
                                scene::ShadowCasterBytes);
  if (FAILED(hr))
    return hr;
  c.frameCounts.admittedBytes = uint32_t(std::min<uint64_t>(c.pool.demand.frameBytes, UINT32_MAX));
  ++c.sent;
  ++c.status.sent;
  c.status.dynamicBytes += (vertexDynamic ? uint64_t(facts.numVertices) * stride : 0u) + sizes.index;
  const CasterClass casterClass = terrainS1 || (terrainTile && !terrainDoodad) ? ClassTerrain
                                : stage10Doodad ? ClassDoodad
                                : (unitProven || stage == 11) ? ClassUnit : ClassWorldObject;
  ++(casterClass == ClassTerrain ? c.status.terrain : casterClass == ClassDoodad ? c.status.doodads
     : casterClass == ClassUnit ? c.status.units : c.status.worldObjects);
  // Counting only: the share of casters an outline could match at all, and
  // how many it does (research note 2026-11-17 §4.2).
  ++c.frameCounts.sent;
  ++(casterClass == ClassTerrain ? c.frameCounts.terrain : casterClass == ClassDoodad ? c.frameCounts.doodads
     : casterClass == ClassUnit ? c.frameCounts.units : c.frameCounts.worldObjects);
  if (batchHandle != 0u) {
    ++c.status.withHandle;
    ++c.frameCounts.withHandle;
    if (casterClass == ClassUnit) {
      ++c.status.unitsWithHandle;
      ++c.frameCounts.unitsWithHandle;
    }
    if (War3RenderState::GetOutlineHandleCount() != 0u && War3RenderState::IsOutlineHandle(batchHandle)) {
      ++c.status.outlineMatched;
      ++c.frameCounts.outlineMatched;
    }
  }
  // An object drawn from a dynamic buffer may have moved or changed its pose
  // since the last frame, and nothing here can tell: the frame's signature
  // changes with every frame it is in, so the pass renders its maps anew.
  if (!terrainCaster && vertexDynamic) {
    ++c.dynamicCasters;
    uint64_t hash = c.signature != 0u ? c.signature : bit::fnv1a_init();
    hash = bit::fnv1a_iter(hash, uint32_t(c.frame));
    hash = bit::fnv1a_iter(hash, batchHandle);
    hash = bit::fnv1a_iter(hash, facts.numVertices);
    c.signature = hash;
  }
  if (c.logged[casterClass] < 6u) {
    ++c.logged[casterClass];
    Logger::info(str::format("WarVK proxy mode: caster class=", uint32_t(casterClass), " stage=", stage,
                             " category=", uint32_t(category), " tag=", int32_t(tag), "/", int32_t(execTag),
                             " kind=", uint32_t(kind), " unit=", unitProven ? 1 : 0,
                             " handle=0x", std::hex, batchHandle, " rawcode=0x", rawcode, std::dec,
                             " vertices=", facts.numVertices, " triangles=", facts.primitiveCount,
                             " stride=", stride, " dynamic=", vertexDynamic ? 1 : 0, indexDynamic ? 1 : 0,
                             " alphaTest=", alphaTest ? 1 : 0, " blend=", alphaBlend ? 1 : 0,
                             " zwrite=", depthWrite ? 1 : 0,
                             " origin=", world._41, ",", world._42, ",", world._43,
                             " radius=", record.boundsRadius));
  }

  if (alphaTest) {
    scene::ShadowCasterAlpha alpha;
    alpha.stage = 0u;
    alpha.alphaRef = float(RenderStateOf(device, D3DRS_ALPHAREF) & 0xFFu) / 255.0f;
    alpha.texcoord = 0u;
    hr = AnnotateNextDraw(scene::ShadowCasterAlphaSchema, scene::EncodeShadowCasterAlpha(alpha).data(),
                          scene::ShadowCasterAlphaBytes);
    if (FAILED(hr))
      return hr;
    ++c.status.alpha;
  }
  return S_OK;
}

CasterFrameAccount CastersFrameAccount() {
  // The frame's casters are complete: its ShadowFrame record closes them.
  c.status.lastFrame = CastersCurrentCounts();
  CasterFrameAccount account;
  account.casters = c.sent;
  account.omitted = c.pool.omitted;
  account.omissionReasons = c.pool.omitted != 0u ? uint32_t(War3RequiredCasterOmissionReason::ArenaAdmission) : 0u;
  account.dynamicCasters = c.dynamicCasters;
  account.dynamicSignature = c.signature;
  return account;
}

void CastersEndFrame() {
  c.sent = c.dynamicCasters = 0u;
  c.signature = 0u;
  c.wanted = false;
  c.pool = {};
  c.frameCounts = {};
}

CasterFrameCounts CastersCurrentCounts() {
  CasterFrameCounts counts = c.frameCounts;
  counts.demandCasters = c.pool.casters;
  counts.demandBytes = c.pool.demand.frameBytes;
  return counts;
}

const CasterStatus& CastersStatus() { return c.status; }

} // namespace dxvk::war3::proxy
