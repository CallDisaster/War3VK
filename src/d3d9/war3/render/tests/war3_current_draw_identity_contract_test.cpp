#include "../war3_current_draw_contract.h"

#include <iostream>

namespace {

using dxvk::war3::render::CurrentDrawContractHasCanonicalIdentity;
using dxvk::war3::render::CurrentDrawContractRecord;

bool require(bool condition, const char* message) {
  if (!condition)
    std::cerr << "war3_current_draw_identity_contract_test: " << message
              << '\n';
  return condition;
}

bool testCanonicalIdentity() {
  CurrentDrawContractRecord record = {};
  if (!require(!CurrentDrawContractHasCanonicalIdentity(record),
               "empty record acquired canonical identity"))
    return false;

  record.known = true;
  if (!require(CurrentDrawContractHasCanonicalIdentity(record),
               "legacy resolved record lost canonical identity"))
    return false;

  record = {};
  record.renderablePart = reinterpret_cast<void*>(uintptr_t(0x1000u));
  if (!require(!CurrentDrawContractHasCanonicalIdentity(record),
               "part-only record acquired canonical identity"))
    return false;

  record.meshPayloadPtr = reinterpret_cast<void*>(uintptr_t(0x2000u));
  if (!require(CurrentDrawContractHasCanonicalIdentity(record),
               "exact part/mesh record was not normalized"))
    return false;

  record.renderablePart = nullptr;
  return require(!CurrentDrawContractHasCanonicalIdentity(record),
                 "mesh-only record acquired canonical identity");
}

} // namespace

int main() {
  return testCanonicalIdentity() ? 0 : 1;
}
