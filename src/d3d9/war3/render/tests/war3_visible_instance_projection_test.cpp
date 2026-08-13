#include "../war3_visible_instance_projection.h"

#include <iostream>

namespace {

using dxvk::war3::render::War3CanProjectVisibleInstance;
using dxvk::war3::render::War3VisibleInstanceProjectionFacts;

bool require(bool condition, const char* message) {
  if (!condition)
    std::cerr << "war3_visible_instance_projection_test: " << message << '\n';
  return condition;
}

War3VisibleInstanceProjectionFacts baseFacts() {
  War3VisibleInstanceProjectionFacts facts = {};
  facts.visibleWorldObjectEntry = reinterpret_cast<void*>(0x1000u);
  facts.visibleSceneNode = reinterpret_cast<void*>(0x2000u);
  facts.visibleIdentitySceneNode = facts.visibleSceneNode;
  facts.visibleUnitPtr = reinterpret_cast<void*>(0x3000u);
  facts.visibleJHandle = 17u;
  facts.visibleRawcode = 0x68666F6Fu;
  facts.visibleRuntimeModelPtr = reinterpret_cast<void*>(0x4000u);
  facts.visibleModelResourcePtr = reinterpret_cast<void*>(0x5000u);
  facts.visibleModelKey = 19u;
  return facts;
}

bool testPartialMatchingStrongAliases() {
  auto facts = baseFacts();
  facts.recordJHandle = facts.visibleJHandle;
  if (!require(War3CanProjectVisibleInstance(facts),
               "matching handle alone should prove the instance"))
    return false;

  facts = baseFacts();
  facts.recordUnitPtr = facts.visibleUnitPtr;
  facts.recordRawcode = facts.visibleRawcode;
  return require(War3CanProjectVisibleInstance(facts),
                 "matching unit/rawcode should prove the instance");
}

bool testSharedPartWithoutInstanceAliasFails() {
  auto facts = baseFacts();
  facts.recordRawcode = facts.visibleRawcode;
  return require(!War3CanProjectVisibleInstance(facts),
                 "rawcode/model owner must not prove a shared instance");
}

bool testAnyConflictFails() {
  auto facts = baseFacts();
  facts.recordJHandle = facts.visibleJHandle;
  facts.recordUnitPtr = reinterpret_cast<void*>(0xDEADu);
  if (!require(!War3CanProjectVisibleInstance(facts),
               "unit conflict was accepted"))
    return false;

  facts = baseFacts();
  facts.recordSceneNode = facts.visibleSceneNode;
  facts.visibleIdentitySceneNode = reinterpret_cast<void*>(0xBADu);
  if (!require(!War3CanProjectVisibleInstance(facts),
               "visible scene aliases disagreed"))
    return false;

  facts = baseFacts();
  facts.recordWorldObjectEntry = facts.visibleWorldObjectEntry;
  facts.recordRawcode = 1u;
  return require(!War3CanProjectVisibleInstance(facts),
                 "rawcode conflict was accepted");
}

bool testOwnerMustBeComplete() {
  auto facts = baseFacts();
  facts.recordJHandle = facts.visibleJHandle;
  facts.visibleModelKey = 0u;
  if (!require(!War3CanProjectVisibleInstance(facts),
               "missing model key was accepted"))
    return false;
  facts = baseFacts();
  facts.recordJHandle = facts.visibleJHandle;
  facts.visibleRuntimeModelPtr = nullptr;
  return require(!War3CanProjectVisibleInstance(facts),
                 "missing runtime model was accepted");
}

} // namespace

int main() {
  return testPartialMatchingStrongAliases() &&
          testSharedPartWithoutInstanceAliasFails() && testAnyConflictFails() &&
          testOwnerMustBeComplete()
      ? 0 : 1;
}
