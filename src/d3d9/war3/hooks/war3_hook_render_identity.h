#pragma once

#include <cstdint>

namespace dxvk::war3::hooks {

class War3HookRenderIdentity {
public:
  static void Install(uintptr_t gameBase);
};

} // namespace dxvk::war3::hooks
