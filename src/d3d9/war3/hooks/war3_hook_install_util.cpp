#include "war3_hook_install_util.h"

#include <MinHook.h>

#include <algorithm>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

#include "../../d3d9_war3_debug.h"

namespace dxvk::war3::hooks {

namespace {

struct HookCatalogEntry {
  War3HookCatalogRecord descriptor;
  War3HookRuntimeState runtime;
};

struct HookInstallRegistry {
  std::mutex mutex;
  std::vector<War3HookInstallRecord> legacyRecords;
  std::vector<HookCatalogEntry> catalog;
};

HookInstallRegistry& GetHookInstallRegistry() {
  // Function-local construction avoids initialization-order dependencies with
  // the many DLL hook domains. Registration itself is always explicit.
  static HookInstallRegistry registry;
  return registry;
}

const char* SafeText(const char* value, const char* fallback = "") {
  return value && value[0] ? value : fallback;
}

War3HookCatalogRecord MakeCatalogDescriptorRecord(
    const War3HookDescriptor& descriptor) {
  War3HookCatalogRecord record;
  record.id = static_cast<uint32_t>(descriptor.id);
  record.domain = SafeText(descriptor.domain, "Unknown");
  record.hookName = SafeText(descriptor.hookName, "UnnamedHook");
  record.kind = War3HookKindName(descriptor.kind);
  record.targetRva = descriptor.targetRva;
  record.timingRoot = SafeText(descriptor.timingRoot);
  record.nativeMode = SafeText(descriptor.nativeMode, "unresolved");
  record.customPhase = SafeText(descriptor.customPhase, "Unresolved");
  record.safetyClass = SafeText(descriptor.safetyClass, "Unclassified");
  record.compileConfig =
      SafeText(descriptor.activationGate.compileConfig);
  record.environment = SafeText(descriptor.activationGate.environment);
  record.minPerfLevel = descriptor.activationGate.minPerfLevel;
  record.runtimeModules =
      SafeText(descriptor.activationGate.runtimeModules);
  record.owner = SafeText(descriptor.activationGate.owner);
  record.displayExpression =
      SafeText(descriptor.activationGate.displayExpression);
  return record;
}

War3HookId FindRegisteredHookId(const char* domain, const char* hookName) {
  auto& registry = GetHookInstallRegistry();
  std::lock_guard<std::mutex> lock(registry.mutex);
  const char* safeDomain = SafeText(domain, "Unknown");
  const char* safeName = SafeText(hookName, "UnnamedHook");
  const auto it = std::find_if(
      registry.catalog.begin(), registry.catalog.end(),
      [&](const HookCatalogEntry& item) {
        return item.descriptor.domain == safeDomain &&
               item.descriptor.hookName == safeName;
      });
  return it == registry.catalog.end()
             ? War3HookId::Invalid
             : static_cast<War3HookId>(it->descriptor.id);
}

bool InstallMinHookImpl(War3HookId hookId, LPVOID target, LPVOID detour,
                        LPVOID* original, const char* domain,
                        const char* hookName, bool ensureInitialized,
                        bool logSuccess);
}

const char* War3HookCatalogStatusName(War3HookCatalogStatus state) {
  switch (state) {
    case War3HookCatalogStatus::NotEvaluated:
      return "NotEvaluated";
    case War3HookCatalogStatus::Installed:
      return "Installed";
    case War3HookCatalogStatus::DisabledByCompileConfig:
      return "DisabledByCompileConfig";
    case War3HookCatalogStatus::DisabledByEnvironment:
      return "DisabledByEnvironment";
    case War3HookCatalogStatus::SkippedUnsafeABI:
      return "SkippedUnsafeABI";
    case War3HookCatalogStatus::AddressUnavailable:
      return "AddressUnavailable";
    case War3HookCatalogStatus::InstallFailed:
      return "InstallFailed";
  }
  return "NotEvaluated";
}

const char* War3HookKindName(War3HookKind kind) {
  switch (kind) {
    case War3HookKind::MinHookDetour:
      return "MinHookDetour";
    case War3HookKind::QueuedGroup:
      return "QueuedGroup";
    case War3HookKind::D3D9Frontend:
      return "D3D9Frontend";
    case War3HookKind::WarVKCallback:
      return "WarVKCallback";
    case War3HookKind::PatchOnly:
      return "PatchOnly";
  }
  return "MinHookDetour";
}

void RecordHookInstallState(const char* domain, const char* hookName,
                            LPVOID target, LPVOID detour,
                            LPVOID trampoline, const char* state,
                            bool installed) {
  War3HookInstallRecord record;
  record.domain = domain && domain[0] ? domain : "Unknown";
  record.hookName = hookName && hookName[0] ? hookName : "UnnamedHook";
  record.state = state && state[0] ? state : "unknown";
  record.target = reinterpret_cast<uintptr_t>(target);
  record.detour = reinterpret_cast<uintptr_t>(detour);
  record.trampoline = reinterpret_cast<uintptr_t>(trampoline);
  record.installed = installed;

  auto& registry = GetHookInstallRegistry();
  std::lock_guard<std::mutex> lock(registry.mutex);
  const auto it = std::find_if(
      registry.legacyRecords.begin(), registry.legacyRecords.end(),
      [&](const War3HookInstallRecord& item) {
        return item.domain == record.domain &&
               item.hookName == record.hookName &&
               (item.target == record.target || item.target == 0u ||
                record.target == 0u);
      });
  if (it != registry.legacyRecords.end())
    *it = std::move(record);
  else
    registry.legacyRecords.push_back(std::move(record));
}

std::vector<War3HookInstallRecord> SnapshotHookInstallRecords() {
  auto& registry = GetHookInstallRegistry();
  std::lock_guard<std::mutex> lock(registry.mutex);
  return registry.legacyRecords;
}

void RegisterHookDescriptors(const War3HookDescriptor* descriptors,
                             size_t descriptorCount) {
  if (!descriptors || descriptorCount == 0u)
    return;

  auto& registry = GetHookInstallRegistry();
  std::lock_guard<std::mutex> lock(registry.mutex);
  for (size_t i = 0u; i < descriptorCount; ++i) {
    const auto& descriptor = descriptors[i];
    if (descriptor.id == War3HookId::Invalid)
      continue;

    const auto id = static_cast<uint32_t>(descriptor.id);
    const auto it = std::find_if(
        registry.catalog.begin(), registry.catalog.end(),
        [&](const HookCatalogEntry& item) {
          return item.descriptor.id == id;
        });
    if (it != registry.catalog.end()) {
      // Metadata may be rebuilt from the address book on a later Install()
      // call. Preserve the independently accumulated runtime state.
      it->descriptor = MakeCatalogDescriptorRecord(descriptor);
      continue;
    }

    HookCatalogEntry entry;
    entry.descriptor = MakeCatalogDescriptorRecord(descriptor);
    entry.runtime.id = descriptor.id;
    registry.catalog.push_back(std::move(entry));
  }
}

void RecordHookCatalogState(War3HookId id, War3HookCatalogStatus state,
                            LPVOID target, LPVOID detour,
                            LPVOID trampoline, int32_t minHookStatus,
                            const char* reason, bool installAttempt) {
  if (id == War3HookId::Invalid)
    return;

  auto& registry = GetHookInstallRegistry();
  std::lock_guard<std::mutex> lock(registry.mutex);
  const uint32_t stableId = static_cast<uint32_t>(id);
  auto it = std::find_if(
      registry.catalog.begin(), registry.catalog.end(),
      [&](const HookCatalogEntry& item) {
        return item.descriptor.id == stableId;
      });
  if (it == registry.catalog.end()) {
    HookCatalogEntry entry;
    entry.descriptor.id = stableId;
    entry.descriptor.domain = "Unknown";
    entry.descriptor.hookName = "UnregisteredDescriptor";
    entry.runtime.id = id;
    registry.catalog.push_back(std::move(entry));
    it = std::prev(registry.catalog.end());
  }

  auto& runtime = it->runtime;
  runtime.lastAttemptState = state;
  if (!runtime.installed || state == War3HookCatalogStatus::Installed) {
    // These addresses describe the active installation. A later failed or
    // disabled attempt is represented by lastAttemptState/minHookStatus and
    // must not clear a process-lifetime trampoline that is still installed.
    runtime.target = reinterpret_cast<uintptr_t>(target);
    runtime.detour = reinterpret_cast<uintptr_t>(detour);
    runtime.trampoline = reinterpret_cast<uintptr_t>(trampoline);
  }
  runtime.minHookStatus = minHookStatus;
  runtime.reason = SafeText(reason);
  if (installAttempt)
    ++runtime.attemptCount;

  // Installed describes process-lifetime truth. A later duplicate install
  // attempt may fail or be disabled, but must not erase the fact that this
  // target is already detoured. lastAttemptState retains that later outcome.
  if (state == War3HookCatalogStatus::Installed) {
    runtime.installed = true;
    runtime.status = War3HookCatalogStatus::Installed;
  } else if (!runtime.installed) {
    runtime.status = state;
  }
}

std::vector<War3HookCatalogRecord> SnapshotHookCatalogRecords() {
  auto& registry = GetHookInstallRegistry();
  std::lock_guard<std::mutex> lock(registry.mutex);
  std::vector<War3HookCatalogRecord> snapshot;
  snapshot.reserve(registry.catalog.size());
  for (const auto& entry : registry.catalog) {
    War3HookCatalogRecord record = entry.descriptor;
    record.status = War3HookCatalogStatusName(entry.runtime.status);
    record.lastAttemptState =
        War3HookCatalogStatusName(entry.runtime.lastAttemptState);
    record.target = entry.runtime.target;
    record.detour = entry.runtime.detour;
    record.trampoline = entry.runtime.trampoline;
    record.minHookStatus = entry.runtime.minHookStatus;
    record.attemptCount = entry.runtime.attemptCount;
    record.installed = entry.runtime.installed;
    record.reason = entry.runtime.reason;
    snapshot.push_back(std::move(record));
  }
  std::sort(snapshot.begin(), snapshot.end(),
            [](const War3HookCatalogRecord& a,
               const War3HookCatalogRecord& b) {
              return a.id < b.id;
            });
  return snapshot;
}

namespace {

bool InstallMinHookImpl(War3HookId hookId, LPVOID target, LPVOID detour,
                        LPVOID* original, const char* domain,
                        const char* hookName, bool ensureInitialized,
                        bool logSuccess) {
  const char* safeDomain = (domain && domain[0]) ? domain : "Unknown";
  const char* safeName = (hookName && hookName[0]) ? hookName : "UnnamedHook";
  const War3HookId effectiveHookId =
      hookId == War3HookId::Invalid
          ? FindRegisteredHookId(safeDomain, safeName)
          : hookId;

  if (!target || !detour || !original) {
    war3dbg::Print(
        "DXVK War3Hook[%s]: Skip %s (invalid args target=%p detour=%p original=%p)\n",
        safeDomain, safeName, target, detour, original);
    RecordHookInstallState(safeDomain, safeName, target, detour, nullptr,
                           "invalid-args", false);
    RecordHookCatalogState(
        effectiveHookId,
        !target ? War3HookCatalogStatus::AddressUnavailable
                : War3HookCatalogStatus::InstallFailed,
        target, detour, nullptr, -1,
        !target ? "target-address-unavailable" : "invalid-install-arguments",
        true);
    return false;
  }

  if (ensureInitialized) {
    const MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED) {
      war3dbg::Print(
          "DXVK War3Hook[%s]: 安装 %s 失败 (MH_Initialize=%d)\n",
          safeDomain, safeName, static_cast<int>(initStatus));
      RecordHookInstallState(safeDomain, safeName, target, detour, nullptr,
                             "initialize-failed", false);
      RecordHookCatalogState(
          effectiveHookId, War3HookCatalogStatus::InstallFailed, target, detour,
          nullptr, static_cast<int32_t>(initStatus),
          "minhook-initialize-failed", true);
      return false;
    }
  }

  const MH_STATUS createStatus = MH_CreateHook(target, detour, original);
  if (createStatus != MH_OK && createStatus != MH_ERROR_ALREADY_CREATED) {
    war3dbg::Print(
        "DXVK War3Hook[%s]: 安装 %s 失败 (MH_CreateHook=%d, addr=%p)\n",
        safeDomain, safeName, static_cast<int>(createStatus), target);
    RecordHookInstallState(safeDomain, safeName, target, detour,
                           original ? *original : nullptr,
                           "create-failed", false);
    RecordHookCatalogState(
        effectiveHookId, War3HookCatalogStatus::InstallFailed, target, detour,
        original ? *original : nullptr, static_cast<int32_t>(createStatus),
        "minhook-create-failed", true);
    return false;
  }

  const MH_STATUS enableStatus = MH_EnableHook(target);
  if (enableStatus != MH_OK && enableStatus != MH_ERROR_ENABLED) {
    war3dbg::Print(
        "DXVK War3Hook[%s]: 启用 %s 失败 (MH_EnableHook=%d, addr=%p)\n",
        safeDomain, safeName, static_cast<int>(enableStatus), target);
    RecordHookInstallState(safeDomain, safeName, target, detour,
                           original ? *original : nullptr,
                           "enable-failed", false);
    RecordHookCatalogState(
        effectiveHookId, War3HookCatalogStatus::InstallFailed, target, detour,
        original ? *original : nullptr, static_cast<int32_t>(enableStatus),
        "minhook-enable-failed", true);
    return false;
  }

  if (logSuccess) {
    war3dbg::Print("DXVK War3Hook[%s]: %s Hook Success (addr=%p)\n", safeDomain,
                   safeName, target);
  }
  RecordHookInstallState(safeDomain, safeName, target, detour,
                         original ? *original : nullptr,
                         "installed", true);
  RecordHookCatalogState(
      effectiveHookId, War3HookCatalogStatus::Installed, target, detour,
      original ? *original : nullptr, static_cast<int32_t>(enableStatus),
      "installed", true);
  return true;
}

}  // namespace

bool InstallMinHook(LPVOID target, LPVOID detour, LPVOID* original,
                    const char* domain, const char* hookName,
                    bool ensureInitialized, bool logSuccess) {
  return InstallMinHookImpl(War3HookId::Invalid, target, detour, original,
                            domain, hookName, ensureInitialized, logSuccess);
}

bool InstallMinHook(War3HookId hookId, LPVOID target, LPVOID detour,
                    LPVOID* original, const char* domain,
                    const char* hookName, bool ensureInitialized,
                    bool logSuccess) {
  return InstallMinHookImpl(hookId, target, detour, original, domain,
                            hookName, ensureInitialized, logSuccess);
}

}  // namespace dxvk::war3::hooks

