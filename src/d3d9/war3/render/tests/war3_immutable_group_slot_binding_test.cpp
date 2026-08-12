#include "../war3_immutable_group_slot_binding.h"

#include <cstdint>
#include <iostream>

namespace {

using dxvk::war3::render::ImmutableGroupSlotBindingProof;
using dxvk::war3::render::ValidateImmutableGroupSlotBinding;

bool require(bool value, const char* message) {
  if (!value)
    std::cerr << "war3_immutable_group_slot_binding_test: " << message << '\n';
  return value;
}

ImmutableGroupSlotBindingProof validProof() {
  static const uint8_t slots[] = {0u, 1u, 2u, 1u};
  static const int owner = 7;
  ImmutableGroupSlotBindingProof proof = {};
  proof.declared = true;
  proof.ownerReady = true;
  proof.keepAliveOwner = &owner;
  proof.resolvedOwner = &owner;
  proof.packetSlots = slots;
  proof.ownerSlots = slots;
  proof.packetMapEpoch = proof.ownerMapEpoch = 3u;
  proof.packetImmutableGeneration = proof.ownerImmutableGeneration = 9u;
  proof.packetGeosetIndex = proof.ownerGeosetIndex = 2u;
  proof.requiredVertexCount = 4u;
  proof.ownerSlotCount = 4u;
  return proof;
}

bool testExactProof() {
  auto proof = validProof();
  if (!require(ValidateImmutableGroupSlotBinding(proof),
               "complete immutable binding proof was rejected"))
    return false;
  proof.packetImmutableGeneration++;
  if (!require(!ValidateImmutableGroupSlotBinding(proof),
               "generation mismatch was accepted"))
    return false;
  proof = validProof();
  proof.ownerSlotCount--;
  if (!require(!ValidateImmutableGroupSlotBinding(proof),
               "short owner range was accepted"))
    return false;
  proof = validProof();
  const uint8_t otherSlots[] = {0u, 1u, 2u, 1u};
  proof.packetSlots = otherSlots;
  if (!require(!ValidateImmutableGroupSlotBinding(proof),
               "different slot storage was accepted"))
    return false;
  proof = validProof();
  proof.declared = false;
  return require(!ValidateImmutableGroupSlotBinding(proof),
                 "undeclared alias was accepted");
}

} // namespace

int main() {
  return testExactProof() ? 0 : 1;
}
