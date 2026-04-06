#include "d3d9_war3_branding.h"

namespace dxvk {

std::atomic<bool> War3VKBranding::s_shown = false;
std::atomic<bool> War3VKBranding::s_initialized = false;
uint32_t War3VKBranding::s_frameCounter = 0;

std::string War3VKBranding::DecryptMessage(uint32_t) {
  return {};
}

bool War3VKBranding::DisplayToLocalPlayer(const char*, float) {
  return false;
}

bool War3VKBranding::VerifyIntegrity() {
  return true;
}

void War3VKBranding::TryShowBrandingMessage(float) {
}

void War3VKBranding::ForceShow() {
  s_shown.store(false, std::memory_order_relaxed);
}

} // namespace dxvk
