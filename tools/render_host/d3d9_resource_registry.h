#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace warvk::host::d9 {
constexpr uint32_t MaxResources = 64;
enum class Type : uint32_t { None, VertexBuffer, IndexBuffer, Texture, VertexShader, PixelShader, Declaration, EventQuery };
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
// existing backend retirement succeeds; the proxy only after the exact reply.
template<uint32_t MaxGeneration = std::numeric_limits<uint32_t>::max()>
class BasicResourceRegistry {
  static_assert(MaxGeneration > 0);
  enum class State : uint8_t { Free, Reserved, Live };
  struct Slot { uint32_t generation = 0; Type type = Type::None; State state = State::Free; };
  std::array<Slot, MaxResources + 1> slots{};
  bool next(ResourceRef ref, Type type, State state) const {
    if (!ValidRef(ref) || !ValidType(type)) return false;
    const auto& slot = slots[ref.id];
    return slot.state == state && slot.generation < MaxGeneration
      && ref.generation == slot.generation + 1
      && (state != State::Reserved || slot.type == type);
  }
public:
  ResourceRef reserve(Type type) {
    if (!ValidType(type)) return {};
    for (uint32_t id = 1; id <= MaxResources; ++id) {
      auto& slot = slots[id];
      if (slot.state == State::Free && slot.generation < MaxGeneration) {
        slot.state = State::Reserved; slot.type = type;
        return {id, slot.generation + 1};
      }
    }
    return {};
  }
  bool cancelReservation(ResourceRef ref, Type type) {
    if (!next(ref, type, State::Reserved)) return false;
    auto& slot = slots[ref.id]; slot.state = State::Free; slot.type = Type::None;
    return true; // A proven no-side-effect rejection does not burn a generation.
  }
  bool commitReservation(ResourceRef ref, Type type) {
    if (!next(ref, type, State::Reserved)) return false;
    slots[ref.id] = {ref.generation, type, State::Live}; return true;
  }
  bool canCreate(ResourceRef ref, Type type) const { return next(ref, type, State::Free); }
  bool commitCreate(ResourceRef ref, Type type) {
    if (!canCreate(ref, type)) return false;
    slots[ref.id] = {ref.generation, type, State::Live}; return true;
  }
  bool matches(ResourceRef ref, Type type) const {
    return ValidRef(ref) && ValidType(type) && slots[ref.id].state == State::Live
      && slots[ref.id].generation == ref.generation && slots[ref.id].type == type;
  }
  bool destroy(ResourceRef ref, Type type) {
    if (!matches(ref, type)) return false;
    auto& slot = slots[ref.id]; slot.state = State::Free; slot.type = Type::None;
    return true; // Keep the generation tombstone, including exhausted values.
  }
  uint32_t liveCount() const {
    uint32_t count = 0; for (const auto& slot : slots) count += slot.state == State::Live; return count;
  }
  uint32_t reservedCount() const {
    uint32_t count = 0; for (const auto& slot : slots) count += slot.state == State::Reserved; return count;
  }
  bool operator==(const BasicResourceRegistry& other) const {
    for (std::size_t i = 0; i < slots.size(); ++i)
      if (slots[i].generation != other.slots[i].generation || slots[i].type != other.slots[i].type
          || slots[i].state != other.slots[i].state) return false;
    return true;
  }
};
using ResourceRegistry = BasicResourceRegistry<>;
} // namespace warvk::host::d9
