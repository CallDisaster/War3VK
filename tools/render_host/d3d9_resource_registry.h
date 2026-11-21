#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <vector>

namespace warvk::host::d9 {
// Game scale: one map keeps thousands of managed textures alive at once (the
// largest measured, 2026-11-17: 15,734, which was 96 % of the earlier bound of
// 16384); the laboratory bound was 64. Both endpoints index tables with it, so
// an id above it never reaches a backend call. Ids are handed out lowest
// first and the tables grow or are scanned only up to the highest id in use,
// so the bound itself costs nothing.
constexpr uint32_t MaxResources = 65536;
enum class Type : uint32_t { None, VertexBuffer, IndexBuffer, Texture, VertexShader, PixelShader, Declaration, EventQuery };
constexpr uint32_t BufferSlots=8,TextureSlots=MaxResources-64,ShaderSlots=4,DeclarationSlots=4;
// The backend chooses a physical slot. Admission only needs its pool's live count.
struct ResourceQuota {
  std::array<uint32_t,5> live{};
  static constexpr uint32_t limits[]={BufferSlots,TextureSlots,ShaderSlots,ShaderSlots,DeclarationSlots};
  static size_t pool(Type type) {
    return type==Type::VertexBuffer||type==Type::IndexBuffer?0u:uint32_t(type)-uint32_t(Type::Texture)+1u;
  }
  bool full(Type type) const {return type!=Type::EventQuery&&live[pool(type)]>=limits[pool(type)];}
  void created(Type type) {if(type!=Type::EventQuery)++live[pool(type)];}
  void destroyed(Type type) {if(type!=Type::EventQuery)--live[pool(type)];}
};
struct ResourceRef {
  uint32_t id = 0, generation = 0;
};
inline bool operator==(ResourceRef a, ResourceRef b) { return a.id == b.id && a.generation == b.generation; }
inline bool operator!=(ResourceRef a, ResourceRef b) { return !(a == b); }
inline bool ValidType(Type type) { return type >= Type::VertexBuffer && type <= Type::EventQuery; }
inline bool ValidRef(ResourceRef ref) { return ref.id && ref.id <= MaxResources && ref.generation; }

// The device connection nonce is the outer identity. Both endpoints use this
// same bounded rule; backend typed slots and GPU serials are NOT resource IDs.
// No GPU completion is inferred here. The host may call destroy only AFTER its
// existing backend retirement succeeds. The proxy needs an exact reply or a
// negotiated FIFO publication. An uncertain connection never recycles a slot.
template<uint32_t MaxGeneration = std::numeric_limits<uint32_t>::max()>
class BasicResourceRegistry {
  static_assert(MaxGeneration > 0);
  enum class State : uint8_t { Free, Reserved, Live };
  struct Slot { uint32_t generation = 0; Type type = Type::None; State state = State::Free; };
  // Index 0 is never a resource. An id beyond the vector was never touched: a
  // free slot of generation 0, which is what a full table would hold there.
  std::vector<Slot> slots = std::vector<Slot>(1);
  const Slot& at(uint32_t id) const {
    static const Slot untouched;
    return id < slots.size() ? slots[id] : untouched;
  }
  // Null only when memory for the table cannot be had; nothing changed then.
  Slot* own(uint32_t id) noexcept {
    if (id >= slots.size()) {
      const std::size_t wanted = std::max<std::size_t>(std::size_t(id) + 1, slots.size() * 2);
      try { slots.resize(std::min<std::size_t>(wanted, std::size_t(MaxResources) + 1)); }
      catch (const std::bad_alloc&) { return nullptr; }
    }
    return &slots[id];
  }
  // No id below it can be reserved now: reserve() still hands out the lowest
  // free id, without rescanning the live prefix for every new resource.
  uint32_t lowestFree = 1;
  bool next(ResourceRef ref, Type type, State state) const {
    if (!ValidRef(ref) || !ValidType(type)) return false;
    const auto& slot = at(ref.id);
    return slot.state == state && slot.generation < MaxGeneration
      && ref.generation == slot.generation + 1
      && (state != State::Reserved || slot.type == type);
  }
public:
  // The host creates the GPU object between canCreate and commitCreate, so a
  // table that had to grow at the commit could fail there for want of memory,
  // with the object already made. A table reserved whole (under 1 MiB) never
  // grows: false here means the session could not start, before anything exists.
  bool reserveAll() noexcept {
    try { slots.reserve(std::size_t(MaxResources) + 1); return true; }
    catch (const std::bad_alloc&) { return false; }
  }
  ResourceRef reserve(Type type) {
    if (!ValidType(type)) return {};
    for (uint32_t id = lowestFree; id <= MaxResources; ++id) {
      if (at(id).state == State::Free && at(id).generation < MaxGeneration) {
        Slot* slot = own(id);
        if (!slot) return {};
        slot->state = State::Reserved; slot->type = type; lowestFree = id;
        return {id, slot->generation + 1};
      }
    }
    lowestFree = MaxResources + 1;
    return {};
  }
  bool cancelReservation(ResourceRef ref, Type type) {
    if (!next(ref, type, State::Reserved)) return false;
    auto& slot = slots[ref.id]; slot.state = State::Free; slot.type = Type::None;
    if (ref.id < lowestFree) lowestFree = ref.id;
    return true; // A proven no-side-effect rejection does not burn a generation.
  }
  bool commitReservation(ResourceRef ref, Type type) {
    if (!next(ref, type, State::Reserved)) return false;
    slots[ref.id] = {ref.generation, type, State::Live}; return true; // a reserved slot exists
  }
  bool canCreate(ResourceRef ref, Type type) const { return next(ref, type, State::Free); }
  bool commitCreate(ResourceRef ref, Type type) {
    if (!canCreate(ref, type)) return false;
    Slot* slot = own(ref.id);
    if (!slot) return false;
    *slot = {ref.generation, type, State::Live}; return true;
  }
  bool matches(ResourceRef ref, Type type) const {
    return ValidRef(ref) && ValidType(type) && at(ref.id).state == State::Live
      && at(ref.id).generation == ref.generation && at(ref.id).type == type;
  }
  bool destroy(ResourceRef ref, Type type) {
    if (!matches(ref, type)) return false;
    auto& slot = slots[ref.id]; slot.state = State::Free; slot.type = Type::None;
    if (ref.id < lowestFree) lowestFree = ref.id;
    return true; // Keep the generation tombstone, including exhausted values.
  }
  uint32_t liveCount() const {
    uint32_t count = 0; for (const auto& slot : slots) count += slot.state == State::Live; return count;
  }
  uint32_t reservedCount() const {
    uint32_t count = 0; for (const auto& slot : slots) count += slot.state == State::Reserved; return count;
  }
  bool operator==(const BasicResourceRegistry& other) const {
    const std::size_t count = std::max(slots.size(), other.slots.size());
    for (std::size_t i = 0; i < count; ++i) {
      const auto& a = at(uint32_t(i)); const auto& b = other.at(uint32_t(i));
      if (a.generation != b.generation || a.type != b.type || a.state != b.state) return false;
    }
    return true;
  }
};
using ResourceRegistry = BasicResourceRegistry<>;
} // namespace warvk::host::d9
