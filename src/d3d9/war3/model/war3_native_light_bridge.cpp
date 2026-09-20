#include "war3_native_light_bridge.h"
#include "../core/war3_memory.h"
#include "../hooks/war3_hook_install_util.h"
#include "../tools/war3_perf_monitor.h"
#include "../../../util/util_env.h"
#include "../../../util/log/log.h"
#include "../../../minhook/include/MinHook.h"
#include <windows.h>
#include <wincrypt.h>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace dxvk::war3::native_light {
namespace {
template<class T> bool Read(uintptr_t p, T& out) {
  return p && SafeCopy(&out, reinterpret_cast<void*>(p), sizeof(out));
}
// Cold-path only: actual owned model bytes, never a re-opened pathname.
bool Sha256(const uint8_t* bytes, size_t count, std::string& text) {
  if (!bytes || !count || count > UINT32_MAX) return false;
  static const HMODULE module = LoadLibraryW(L"advapi32.dll");
  static const auto acquire = reinterpret_cast<decltype(&CryptAcquireContextW)>(GetProcAddress(module, "CryptAcquireContextW"));
  static const auto create = reinterpret_cast<decltype(&CryptCreateHash)>(GetProcAddress(module, "CryptCreateHash"));
  static const auto update = reinterpret_cast<decltype(&CryptHashData)>(GetProcAddress(module, "CryptHashData"));
  static const auto finish = reinterpret_cast<decltype(&CryptGetHashParam)>(GetProcAddress(module, "CryptGetHashParam"));
  static const auto destroy = reinterpret_cast<decltype(&CryptDestroyHash)>(GetProcAddress(module, "CryptDestroyHash"));
  static const auto release = reinterpret_cast<decltype(&CryptReleaseContext)>(GetProcAddress(module, "CryptReleaseContext"));
  if (!acquire || !create || !update || !finish || !destroy || !release) return false;
  HCRYPTPROV provider = 0; HCRYPTHASH hash = 0; BYTE digest[32] = {}; DWORD length = 32;
  const bool ok = acquire(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) &&
    create(provider, CALG_SHA_256, 0, 0, &hash) && update(hash, bytes, DWORD(count), 0) &&
    finish(hash, HP_HASHVAL, digest, &length, 0) && length == 32;
  if (hash) destroy(hash);
  if (provider) release(provider, 0);
  if (!ok) return false;
  text.clear(); text.reserve(64);
  constexpr char hex[] = "0123456789ABCDEF";
  for (auto v : digest) { text += hex[v>>4]; text += hex[v&15]; }
  return true;
}

using LoadFn = int(__fastcall*)(const char*, uint32_t*, uint32_t*, int);
using DocumentFn = int(__fastcall*)(void*, void*, void*, void*);
using ParseFn = int(__fastcall*)(void*, uint32_t, void*, void*);
using TemplateFn = void*(__fastcall*)(void*, int, void*);
using BuildLightsFn = int(__fastcall*)(void*, void*);
using DestroyModelFn = int(__thiscall*)(void*);
using DestroyLightFn = void(__thiscall*)(void*);
using EgressFn = void(__fastcall*)(uint32_t, void*);
using EnableFn = void(__fastcall*)(uint32_t, uint32_t);
using SelectFn = void(__fastcall*)(uint32_t, int, float, float, int);
using FlushLightFn = uintptr_t(__thiscall*)(void*, uint32_t, const D3DLIGHT9*, uint32_t);
FlushLightFn originalFlushLight = nullptr;
LoadFn originalLoad = nullptr;
DocumentFn originalDocument = nullptr;
ParseFn originalParse = nullptr;
TemplateFn originalTemplate = nullptr;
BuildLightsFn originalBuildLights = nullptr;
DestroyModelFn originalDestroyModel = nullptr;
DestroyLightFn originalDestroyLight = nullptr;
EgressFn originalEgress = nullptr;
EnableFn originalEnable = nullptr;
// The selected slot cache is retained explicitly; E3410 is not called again
// for unchanged native buckets. Collection runs after the selector itself.
SelectFn originalSelect = nullptr;

struct Parsed { ModelLightSources sources; uint64_t generation = 0; };
struct Model {
  ModelLightSources sources;
  std::array<uint32_t,16> lights = {};
  std::array<uint64_t,16> generations = {};
  bool instance = false;
};
struct Owner { uint32_t model = 0; uint64_t generation = 0; uint32_t index = 0; };
struct Catalog { std::string path, sha; };
struct Rule { bool enabled=false, shadows=false; };
struct State {
  std::mutex mutex;
  Generation generation;
  std::unordered_map<uint32_t, Parsed> documents;
  std::unordered_map<uint32_t, Model> models;
  std::unordered_map<uint32_t, Owner> lights;
  std::vector<Catalog> catalogs;
  std::unordered_map<std::string, Rule> rules;
  uint64_t policyGeneration = 1;
  uint64_t epoch = 1, frame = 0;
  Frame collecting, published;
  uint64_t loads = 0, parses = 0, templates = 0, clones = 0, selections = 0;
  uint64_t rejected = 0, destroyed = 0, overflows = 0;
};
State& S() { static State* s = new State; return *s; } // OS owns final teardown

// Called only while holding State::mutex. Unregistered custom content remains
// canonical native FFP. Built-ins require their frozen content hash, not a path
// alias. Explicit map rules may authorize the actual newly imported content.
bool Allowed(const State& s, const Source& source, bool shadow) {
  if (source.catalog >= s.catalogs.size()) return false;
  const auto rule=s.rules.find(s.catalogs[source.catalog].path);
  if (rule != s.rules.end()) return rule->second.enabled && (!shadow || rule->second.shadows);
  return source.policyIndex != UINT32_MAX;
}
void EraseModel(State& s, uint32_t key) {
  const auto model=s.models.find(key);
  if (model==s.models.end()) return;
  for (uint32_t i=0;i<model->second.sources.count;++i) {
    const auto light=s.lights.find(model->second.lights[i]);
    if (light!=s.lights.end() && light->second.model==key &&
        light->second.generation==model->second.generations[i]) s.lights.erase(light);
  }
  s.models.erase(model);
}
uintptr_t base = 0;
std::atomic<bool> installed{false};
struct LoadScope {
  uint32_t document = 0, buffer = 0, bytes = 0;
  std::array<char, 260> path = {};
  Parsed parsed;
  bool parsedOk = false;
};
thread_local std::array<LoadScope, 8> loadStack;
thread_local uint32_t loadDepth = 0;
struct Slot {
  uint32_t light = 0;
  uint64_t generation = 0;
  bool enabled = false;
  NativeValue copied = {};
};
thread_local std::array<Slot, 8> slots = {};
struct Binding { Slot owner; D3DLIGHT9 value = {}; bool valid = false; };
thread_local std::array<Binding, 8> bindings = {};
thread_local bool world = false;
thread_local uint64_t worldSerial = 0;

bool ReadTable(uint32_t model, uint32_t& count, uint32_t& array) {
  uint32_t vt = 0, flags = 0, cap = 0;
  return Read(model, vt) && (vt == base+0x96174c || vt == base+0x96175c) &&
    Read(model+0x94, flags) && (flags&0x10) && Read(model+0xb4, cap) &&
    Read(model+0xb8, count) && Read(model+0xbc, array) &&
    count && count <= 16 && count <= cap && array && array <= UINT32_MAX-64;
}

int __fastcall Load(const char* path, uint32_t* output, uint32_t* size, int flags) {
  const int result = originalLoad(path, output, size, flags);
  if (!Enabled() || !loadDepth || loadDepth > loadStack.size()) return result;
  auto& scope = loadStack[loadDepth-1];
  scope.buffer = scope.bytes = 0;
  if (!result || !output || !size || !Read(uintptr_t(output), scope.buffer) ||
      !Read(uintptr_t(size), scope.bytes)) return result;
  bool pathOk = false;
  for (size_t i = 0; i < scope.path.size(); ++i) {
    if (!Read(uintptr_t(path)+i, scope.path[i])) break;
    if (!scope.path[i]) { pathOk = i != 0; break; }
  }
  if (!pathOk) scope.buffer = scope.bytes = 0;
  return result;
}

int __fastcall Parse(void* reader, uint32_t size, void* document, void* diagnostic) {
  Parsed parsed;
  bool reviewed = false;
  if (Enabled() && loadDepth && loadDepth <= loadStack.size()) {
    auto timing = War3PerfMonitor::instance().cpuScope("NativeModelLights/LoadProof");
    auto& scope = loadStack[loadDepth-1];
    uint32_t buffer = 0;
    std::string path;
    if (size <= 4u*1024u*1024u && NormalizeModelLightPath(scope.path.data(),path) &&
        scope.document == uintptr_t(document) && scope.bytes == size &&
        Read(uintptr_t(reader)+0x10, buffer) && buffer == scope.buffer) {
      std::vector<uint8_t> owned(size);
      std::string sha;
      reviewed = SafeCopy(owned.data(), reinterpret_cast<void*>(buffer), size) &&
        DecodeModelLightSources(owned.data(), owned.size(), parsed.sources) &&
        Sha256(owned.data(), owned.size(), sha);
      auto& state = S(); std::lock_guard<std::mutex> lock(state.mutex);
      ++state.loads;
      if (reviewed) {
        uint32_t catalog=0;
        for (;catalog<state.catalogs.size();++catalog)
          if (state.catalogs[catalog].path==path && state.catalogs[catalog].sha==sha) break;
        if (catalog==state.catalogs.size()) {
          if (catalog>=256) { ++state.overflows; reviewed=false; }
          else state.catalogs.push_back({path,sha});
        }
        Source builtin;
        const bool frozen=DecodeReviewedSource(owned.data(),owned.size(),scope.path.data(),sha,builtin);
        for (uint32_t i=0;reviewed && i<parsed.sources.count;++i) {
          auto& light=parsed.sources.lights[i]; light.catalog=catalog;
          if (frozen && builtin.objectId==light.objectId && builtin.ordinal==light.ordinal)
            light.policyIndex=builtin.policyIndex;
          Logger::info(str::format("NativeModelLights source path=",path,
            " sha256=",sha," bytes=",size," objectId=",light.objectId,
            " ordinal=",light.ordinal," range=",light.range," registered=",Allowed(state,light,false)));
        }
        if (reviewed) parsed.generation=state.generation.next();
      }
    }
  }
  const int result = originalParse(reader, size, document, diagnostic);
  if (Enabled() && loadDepth && loadDepth <= loadStack.size()) {
    auto& scope = loadStack[loadDepth-1];
    if (scope.document == uintptr_t(document)) {
      scope.parsed = parsed;
      scope.parsedOk = result && reviewed && parsed.generation;
      scope.buffer = scope.bytes = 0; // one exact parse consumption, no reuse
    }
  }
  return result;
}

int __fastcall Document(void* path, void* scratch, void* document, void* diagnostic) {
  if (!Enabled()) return originalDocument(path, scratch, document, diagnostic);
  auto& state = S();
  { std::lock_guard<std::mutex> lock(state.mutex); state.documents.erase(uintptr_t(document)); }
  ++loadDepth;
  if (loadDepth <= loadStack.size()) {
    loadStack[loadDepth-1] = {};
    loadStack[loadDepth-1].document = uintptr_t(document);
  }
  const int result = originalDocument(path, scratch, document, diagnostic);
  if (loadDepth <= loadStack.size()) {
    const auto proof = loadStack[loadDepth-1];
    loadStack[loadDepth-1] = {};
    if (result && proof.parsedOk) {
      std::lock_guard<std::mutex> lock(state.mutex);
      if (state.documents.size() < 128) {
        state.documents[uintptr_t(document)] = proof.parsed; ++state.parses;
      } else ++state.overflows;
    }
  }
  --loadDepth;
  return result;
}

int __fastcall BuildLights(void* document, void* model) {
  const int result = originalBuildLights(document, model);
  if (!Enabled()) return result;
  auto& state = S(); std::lock_guard<std::mutex> lock(state.mutex);
  auto it = state.documents.find(uintptr_t(document));
  EraseModel(state,uintptr_t(model));
  if (!result || it == state.documents.end()) return result;
  const auto& proof = it->second;
  uint32_t count = 0, array = 0, parsedCount = 0, parsedArray = 0;
  // Parsed Node base has ObjectID at +80; verify independently from MDX ordinal.
  if (!proof.sources.count || !ReadTable(uintptr_t(model), count, array) || count != proof.sources.lights[0].count ||
      !Read(uintptr_t(document)+0x30e0, parsedCount) || parsedCount != count ||
      !Read(uintptr_t(document)+0x30e4, parsedArray) ||
      parsedArray > UINT32_MAX-376u*count || state.models.size() >= 4096) {
    ++state.rejected; return result;
  }
  Model resultModel; resultModel.sources=proof.sources;
  for (uint32_t i=0;i<proof.sources.count;++i) {
    const auto& source=proof.sources.lights[i]; uint32_t id=0;
    if (!Read(parsedArray+376*source.ordinal+80,id) || id!=source.objectId ||
        !Read(array+4*source.ordinal,resultModel.lights[i]) || !resultModel.lights[i]) {
      ++state.rejected; return result;
    }
  }
  state.models[uintptr_t(model)]=resultModel; ++state.templates;
  return result;
}

void* __fastcall Template(void* document, int flags, void* diagnostic) {
  void* result = originalTemplate(document, flags, diagnostic);
  if (Enabled()) {
    auto& state = S(); std::lock_guard<std::mutex> lock(state.mutex);
    state.documents.erase(uintptr_t(document));
  }
  return result;
}

int __fastcall DestroyModel(void* model, void*) {
  if (Enabled()) {
    auto& state = S(); std::lock_guard<std::mutex> lock(state.mutex);
    if (state.models.count(uintptr_t(model))) { EraseModel(state,uintptr_t(model)); ++state.destroyed; }
  }
  return originalDestroyModel(model);
}
void __fastcall DestroyLight(void* light, void*) {
  if (Enabled()) {
    auto& state = S(); std::lock_guard<std::mutex> lock(state.mutex);
    state.lights.erase(uintptr_t(light));
  }
  originalDestroyLight(light);
}
void __fastcall Egress(uint32_t index, void* light) {
  if (Enabled() && index < slots.size()) {
    auto& state = S(); std::lock_guard<std::mutex> lock(state.mutex);
    const auto found = state.lights.find(uintptr_t(light));
    slots[index] = {uint32_t(uintptr_t(light)),
      found == state.lights.end() ? 0 : found->second.generation, true};
    if (!Read(uintptr_t(light), slots[index].copied)) slots[index].generation = 0;
  }
  originalEgress(index, light); // no native lighting suppression in this stage
}
void __fastcall Enable(uint32_t index, uint32_t enabled) {
  if (Enabled() && index < slots.size()) slots[index].enabled = enabled != 0;
  originalEnable(index, enabled);
}
uintptr_t __fastcall FlushLight(void* device, void*, uint32_t index,
    const D3DLIGHT9* value, uint32_t enabled) {
  Binding binding;
  static_assert(sizeof(D3DLIGHT9) == 0x68, "Native cache copies 26 DWORDs");
  if (Enabled() && index < bindings.size() && slots[index].generation &&
      slots[index].enabled && enabled && Read(uintptr_t(value), binding.value)) {
    binding.owner = slots[index]; binding.valid = binding.value.Type == D3DLIGHT_POINT;
  }
  const auto result = originalFlushLight(device, index, value, enabled);
  if (Enabled() && index < bindings.size()) bindings[index] = binding;
  return result;
}
void __fastcall Select(uint32_t limit, int unused, float x, float y, int a5) {
  originalSelect(limit, unused, x, y, a5);
  if (!Enabled() || !world) return;
  auto timing = War3PerfMonitor::instance().cpuScope("NativeModelLights/Select");
  auto& state = S(); std::lock_guard<std::mutex> lock(state.mutex);
  if (worldSerial != state.frame) return;
  ++state.selections;
  for (uint32_t i = 0; i < slots.size() && i < limit; ++i) {
    if (!slots[i].enabled || !slots[i].generation) continue;
    const uint32_t light = slots[i].light;
    auto owner = state.lights.find(light);
    if (owner == state.lights.end() || owner->second.generation != slots[i].generation) continue;
    auto model = state.models.find(owner->second.model);
    if (model == state.models.end() || !model->second.instance ||
        owner->second.index>=model->second.sources.count ||
        model->second.generations[owner->second.index] != owner->second.generation ||
        model->second.lights[owner->second.index] != light) continue;
    const auto& source=model->second.sources.lights[owner->second.index];
    if (!Allowed(state,source,true)) continue;
    auto& frame = state.collecting;
    bool seen = false;
    for (uint32_t j = 0; j < frame.count; ++j)
      if (frame.lights[j].generation == owner->second.generation) seen = true;
    if (seen) continue;
    NativeValue value;
    if (!Read(light, value) || !Usable(value)) continue;
    if (frame.count == frame.lights.size()) { frame.complete = false; ++state.overflows; continue; }
    frame.lights[frame.count++] = {source, value,
      owner->second.generation, state.epoch, state.frame, light, owner->second.model,state.policyGeneration,
      state.rules.count(state.catalogs[source.catalog].path) != 0};
  }
}
} // namespace

bool Enabled() noexcept { return installed.load(std::memory_order_relaxed); }

bool SetModelPath(std::string_view path, bool enabled, bool shadows) {
  std::string normalized;
  if (!Enabled() || !NormalizeModelLightPath(path,normalized)) return false;
  auto& state=S(); std::lock_guard<std::mutex> lock(state.mutex);
  auto old=state.rules.find(normalized);
  if (old!=state.rules.end() && old->second.enabled==enabled && old->second.shadows==shadows) return true;
  if ((old==state.rules.end() && state.rules.size()>=64) || state.policyGeneration==UINT64_MAX) return false;
  state.rules[normalized]={enabled,shadows}; ++state.policyGeneration;
  Logger::info(str::format("NativeModelLights path rule path=",normalized,
    " enabled=",enabled," shadows=",shadows," generation=",state.policyGeneration));
  return true;
}

int32_t ModelPathCount(std::string_view path) {
  std::string normalized;
  if (!Enabled() || !NormalizeModelLightPath(path,normalized)) return -1;
  auto& state=S(); std::lock_guard<std::mutex> lock(state.mutex);
  int32_t count=0;
  for (const auto& entry:state.lights) {
    const auto& owner=entry.second;
    const auto model=state.models.find(owner.model);
    if (model==state.models.end() || !model->second.instance || owner.index>=model->second.sources.count ||
        model->second.generations[owner.index]!=owner.generation) continue;
    const auto& source=model->second.sources.lights[owner.index];
    if (Allowed(state,source,false) && state.catalogs[source.catalog].path==normalized) ++count;
  }
  return count;
}

bool ModelPathRegistered(std::string_view path) {
  std::string normalized;
  if (!Enabled() || !NormalizeModelLightPath(path,normalized)) return false;
  auto& state=S(); std::lock_guard<std::mutex> lock(state.mutex);
  const auto rule=state.rules.find(normalized);
  if (rule!=state.rules.end()) return rule->second.enabled;
  for (const auto& p:render::kNativeModelLightShadowCandidates)
    if (render::NativeLightSamePath(normalized,p.path)) return true;
  return false;
}

uint64_t CurrentWorldSerial() noexcept { return Enabled() && world ? worldSerial : 0; }

bool ClaimDrawLight(uint32_t index, const D3DLIGHT9& value, Sample& out) noexcept {
  if (!CurrentWorldSerial() || index >= bindings.size()) return false;
  const auto& binding = bindings[index];
  if (!binding.valid || !slots[index].enabled ||
      binding.owner.generation != slots[index].generation ||
      binding.owner.light != slots[index].light ||
      std::memcmp(&value, &binding.value, sizeof(value))) return false;
  auto& state = S(); std::lock_guard<std::mutex> lock(state.mutex);
  if (worldSerial != state.frame || !state.collecting.complete) return false;
  auto owner = state.lights.find(binding.owner.light);
  if (owner == state.lights.end() || owner->second.generation != binding.owner.generation) return false;
  auto model = state.models.find(owner->second.model);
  if (model == state.models.end() || !model->second.instance ||
      owner->second.index>=model->second.sources.count ||
      model->second.generations[owner->second.index] != owner->second.generation) return false;
  const auto& source=model->second.sources.lights[owner->second.index];
  if (!Allowed(state,source,true)) return false;
  NativeValue current;
  if (!Read(binding.owner.light, current) || !Usable(current) ||
      std::memcmp(&current, &binding.owner.copied, 36)) return false;
  out = {source, current, owner->second.generation,
    state.epoch, state.frame, binding.owner.light, owner->second.model,state.policyGeneration,
    state.rules.count(state.catalogs[source.catalog].path) != 0};
  return true;
}

bool ValidateLease(const Sample* lights, uint32_t count, uint64_t serial) noexcept {
  if (!Enabled() || !lights || !count || count > 2) return false;
  auto& state = S(); std::lock_guard<std::mutex> lock(state.mutex);
  if (!serial || serial != state.frame || !state.collecting.complete) return false;
  for (uint32_t i = 0; i < count; ++i) {
    const auto& sample = lights[i];
    auto owner = state.lights.find(sample.nativeLight);
    NativeValue current;
    if (sample.epoch != state.epoch || sample.frame != serial ||
        sample.policyGeneration != state.policyGeneration || !Allowed(state,sample.source,true) ||
        owner == state.lights.end() || owner->second.generation != sample.generation ||
        !sample.nativeModel || owner->second.model != sample.nativeModel ||
        !Read(sample.nativeLight, current) || !Usable(current) ||
        std::memcmp(&current, &sample.value, 36)) return false;
  }
  return true;
}

uint64_t EmitterGenerationForModel(const void* model) noexcept {
  if (!Enabled() || !model) return 0;
  auto& state = S(); std::lock_guard<std::mutex> lock(state.mutex);
  const auto found = state.models.find(uintptr_t(model));
  if (found == state.models.end() || !found->second.instance || found->second.sources.count!=1 ||
      found->second.sources.lights[0].policyIndex != 8u) return 0;
  const auto light = state.lights.find(found->second.lights[0]);
  return light != state.lights.end() && light->second.model == uintptr_t(model) &&
      light->second.generation == found->second.generations[0] ? found->second.generations[0] : 0;
}

void CloneInstance(void* instance, void* source, void* result) noexcept {
  if (!Enabled()) return;
  auto timing = War3PerfMonitor::instance().cpuScope("NativeModelLights/Bind");
  auto& state = S(); std::lock_guard<std::mutex> lock(state.mutex);
  const auto key = uint32_t(uintptr_t(instance));
  EraseModel(state,key);
  auto from = state.models.find(uintptr_t(source));
  if (!result || result != instance || from == state.models.end()) return;
  const auto proof = from->second.sources;
  uint32_t count = 0, array = 0;
  if (!proof.count || !ReadTable(key, count, array) || count != proof.lights[0].count ||
      state.models.size() >= 4096 || state.lights.size()+proof.count > 4096) {
    ++state.rejected; return;
  }
  Model instanceModel; instanceModel.sources=proof; instanceModel.instance=true;
  for (uint32_t i=0;i<proof.count;++i) {
    auto& light=instanceModel.lights[i];
    if (!Read(array+4*proof.lights[i].ordinal,light) || !light || light==from->second.lights[i] || state.lights.count(light)) {
      ++state.rejected; return;
    }
    for (uint32_t j=0;j<i;++j) if (light==instanceModel.lights[j]) { ++state.rejected; return; }
    instanceModel.generations[i]=state.generation.next();
    if (!instanceModel.generations[i]) return;
  }
  state.models[key]=instanceModel;
  for (uint32_t i=0;i<proof.count;++i)
    state.lights[instanceModel.lights[i]]={key,instanceModel.generations[i],i};
  ++state.clones;
}

void BeginWorld() noexcept {
  if (!Enabled()) return;
  auto& state = S(); std::lock_guard<std::mutex> lock(state.mutex);
  if (world || state.frame == UINT64_MAX) { world = false; state.collecting.complete = false; return; }
  world = true; worldSerial = ++state.frame;
  state.collecting = {}; state.collecting.epoch = state.epoch;
  state.collecting.serial = state.frame; state.collecting.complete = true;
}
void EndWorld() noexcept {
  if (!Enabled()) return;
  auto& state = S(); std::lock_guard<std::mutex> lock(state.mutex);
  if (!world || worldSerial != state.frame) { state.published = {}; return; }
  state.published = state.collecting; world = false;
  if (state.frame % 120 == 1 || (state.published.count && state.frame < 240))
    Logger::info(str::format("NativeModelLights census frame=", state.frame,
      " epoch=", state.epoch, " lights=", state.published.count,
      " complete=", state.published.complete, " loads=", state.loads,
      " parses=", state.parses, " templates=", state.templates, " clones=", state.clones,
      " selected=", state.selections, " rejects=", state.rejected,
      " overflow=", state.overflows, " destroyed=", state.destroyed,
      " lightingAuthorized=0 shadowAuthorized=0"));
}
void ResetMap() noexcept {
  if (!Enabled()) return;
  auto& state = S(); std::lock_guard<std::mutex> lock(state.mutex);
  state.documents.clear(); state.models.clear(); state.lights.clear();
  state.catalogs.clear(); state.rules.clear();
  if (state.policyGeneration==UINT64_MAX) installed.store(false); else ++state.policyGeneration;
  state.collecting = {}; state.published = {};
  if (state.epoch == UINT64_MAX) installed.store(false); else ++state.epoch;
  world = false; slots = {}; bindings = {};
}
Frame Snapshot() {
  auto& state = S(); std::lock_guard<std::mutex> lock(state.mutex);
  return state.published;
}

bool Install(uintptr_t gameBase) {
  static bool attempted = false;
  if (attempted) return Enabled();
  attempted = true;
  if (env::getEnvVar("DXVK_WAR3_NATIVE_MODEL_LIGHTS") != "1") return false;
  wchar_t path[MAX_PATH] = {};
  if (!GetModuleFileNameW(reinterpret_cast<HMODULE>(gameBase), path, MAX_PATH)) return false;
  HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER size = {}; DWORD read = 0;
  std::vector<uint8_t> bytes(13187048);
  bool ok = GetFileSizeEx(file, &size) && size.QuadPart == int64_t(bytes.size()) &&
    ReadFile(file, bytes.data(), DWORD(bytes.size()), &read, nullptr) && read == bytes.size();
  CloseHandle(file);
  std::string sha;
  ok = ok && Sha256(bytes.data(), bytes.size(), sha) &&
    sha == "E04D1716603C075EB0C8E1E21CF1093A664ADC5249EFAB396BFA08D7B09D0C3A";
  if (!ok || gameBase > UINT32_MAX-0xbc5424u) { Logger::warn("NativeModelLights rejected Game identity/base"); return false; }
  base = gameBase;
  struct Hook { uint32_t rva; void* replacement; void** original; std::array<uint8_t, 5> entry; };
  const std::array<Hook, 11> hooks = {{
    {0x48c10, (void*)&Load, (void**)&originalLoad, {0x55,0x8b,0xec,0x51,0x53}},
    {0x81ab30, (void*)&Document, (void**)&originalDocument, {0x55,0x8b,0xec,0x6a,0xff}},
    {0x819a80, (void*)&Parse, (void**)&originalParse, {0x55,0x8b,0xec,0x83,0xec}},
    {0x1261d0, (void*)&Template, (void**)&originalTemplate, {0x55,0x8b,0xec,0x53,0x8b}},
    {0x13daa0, (void*)&BuildLights, (void**)&originalBuildLights, {0x55,0x8b,0xec,0x83,0xec}},
    {0x1307b0, (void*)&DestroyModel, (void**)&originalDestroyModel, {0x55,0x8b,0xec,0x6a,0xff}},
    {0xcc530, (void*)&DestroyLight, (void**)&originalDestroyLight, {0x8b,0x15,0x50,0xa1,0xbb}},
    {0xe3410, (void*)&Egress, (void**)&originalEgress, {0x56,0x8b,0xf1,0x8b,0x0d}},
    {0xe33f0, (void*)&Enable, (void**)&originalEnable, {0x56,0x8b,0xf1,0x8b,0x0d}},
    {0xcc650, (void*)&Select, (void**)&originalSelect, {0x55,0x8b,0xec,0x83,0xe4}},
    {0xef2d0, (void*)&FlushLight, (void**)&originalFlushLight, {0x55,0x8b,0xec,0x83,0xec}},
  }};
  for (const auto& h : hooks) {
    std::array<uint8_t, 8> actual = {}, expected = {}, preferred = {};
    // Pin the researched instruction prefix as well as the module hash. Then
    // compare the loaded bytes against exactly PE-relocated source bytes.
    if (!RelocatedEntry(bytes.data(), bytes.size(), 0x6f000000u, h.rva, preferred) ||
        !std::equal(h.entry.begin(), h.entry.end(), preferred.begin()) ||
        !RelocatedEntry(bytes.data(), bytes.size(), uint32_t(base), h.rva, expected) ||
        !Read(base+h.rva, actual) || actual != expected) {
      Logger::warn(str::format("NativeModelLights entry mismatch rva=", h.rva)); return false;
    }
  }
  size_t created = 0;
  for (const auto& h : hooks) {
    if (MH_CreateHook((void*)(base+h.rva), h.replacement, h.original) != MH_OK) break;
    ++created;
  }
  if (created == hooks.size()) {
    for (const auto& h : hooks) if (MH_QueueEnableHook((void*)(base+h.rva)) != MH_OK) ok = false;
    if (ok) ok = MH_ApplyQueued() == MH_OK;
  } else ok = false;
  if (!ok) {
    for (size_t i=0; i<created; ++i) { MH_DisableHook((void*)(base+hooks[i].rva)); MH_RemoveHook((void*)(base+hooks[i].rva)); }
    Logger::warn("NativeModelLights hooks rejected; canonical lighting retained"); return false;
  }
  installed.store(true);
  Logger::info("NativeModelLights provenance installed; consumer not yet enabled");
  return true;
}
} // namespace dxvk::war3::native_light
