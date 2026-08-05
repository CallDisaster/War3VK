#pragma once

#include <cstdint>

namespace dxvk::war3::hooks {

struct RenderIdentityLifecycleProbeSummary {
  bool fullDiagnostics = false;
  bool worldObjectListEntryWriteProbeHookInstalled = false;
  bool worldObjectEntryRenderContextHookInstalled = false;
  bool worldObjectEntryRenderPrePostProbeEnabled = false;
  bool renderQueueIdentityPrimingHookInstalled = false;
  uint64_t worldObjectEntryRenderCallCount = 0;
  uint64_t worldObjectEntryRenderSceneNodeReadyBeforeCount = 0;
  uint64_t worldObjectEntryRenderSceneNodeReadyAfterCount = 0;
  uint64_t worldObjectEntryRenderSceneNodeFilledByCallCount = 0;
  uint64_t worldObjectEntryRenderSceneNodeChangedCount = 0;
  uint64_t worldObjectEntryRenderKnownListOwnerHintZeroCount = 0;
  uint64_t worldObjectEntryRenderKnownListOwnerHintNonzeroCount = 0;
  uint64_t worldObjectEntryRenderUnknownListOwnerHintCount = 0;
  uint64_t worldObjectListEntryWriteCallCount = 0;
  uint64_t worldObjectListEntryWriteOwnerHintZeroCount = 0;
  uint64_t worldObjectListEntryWriteOwnerHintNonzeroCount = 0;
  uint64_t worldObjectListEntryWriteOwnerHintHandleCount = 0;
  uint64_t worldObjectListEntryWriteOwnerHintUnitPtrCount = 0;
  uint64_t lastWorldObjectEntryRenderEntryPtr = 0;
  uint64_t lastWorldObjectEntryRenderResolvedListOwnerHintValue = 0;
  uint64_t lastWorldObjectListEntryWriteListPtr = 0;
  uint64_t lastWorldObjectListEntryWriteWorldObjectEntryPtr = 0;
  uint64_t lastWorldObjectListEntryWriteOwnerHintValue = 0;
  uint64_t lastWorldObjectEntryRenderSceneNodeBeforePtr = 0;
  uint64_t lastWorldObjectEntryRenderSceneNodeAfterPtr = 0;
};

class War3HookRenderIdentity {
public:
  static void Install(uintptr_t gameBase);
};

RenderIdentityLifecycleProbeSummary QueryRenderIdentityLifecycleProbeSummary();

} // namespace dxvk::war3::hooks
