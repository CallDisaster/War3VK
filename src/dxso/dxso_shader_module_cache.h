#pragma once

#include "dxso_module.h"
#include "dxso_modinfo.h"
#include "../dxvk/dxvk_shader.h"
#include "../dxvk/dxvk_shader_key.h"

#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace dxvk {

  // Immutable compilation domain. A cache belongs to exactly one device and
  // one effective compiler configuration, never to a shader slot or resource ID.
  struct DxsoShaderModuleDomain {
    const void* deviceIdentity = nullptr;
    DxsoModuleInfo moduleInfo;
    D3D9ConstantLayout vertexLayout{};
    D3D9ConstantLayout pixelLayout{};

    bool matches(const DxsoShaderModuleDomain& other) const;
  };

  // Returned only through shared_ptr<const ...>. Raw sampler masks are kept
  // here; the D3D9 device adapter owns the VS slot-number translation.
  struct DxsoCompiledModule {
    Rc<DxvkShader> shader;
    DxsoIsgn isgn;
    uint32_t usedSamplers = 0, usedRTs = 0, textureTypes = 0;
    DxsoProgramInfo info;
    DxsoShaderMetaInfo meta;
    DxsoDefinedConstants constants;
    int32_t maxDefinedFloat = -1, maxDefinedInt = -1, maxDefinedBool = -1;
  };

  struct DxsoShaderModuleCacheStats {
    uint64_t requests = 0, hits = 0, compileAttempts = 0, compilesSucceeded = 0;
    uint64_t registrations = 0, raceLosers = 0, validationFailures = 0;
    uint64_t registrationFailures = 0, readyEntries = 0, failedEntries = 0;
    // Exact owned token storage, NOT total shader/pipeline/driver memory.
    uint64_t keyBytes = 0, keyCapacityBytes = 0;
    uint64_t constantBytes = 0, constantCapacityBytes = 0;
  };

  class DxsoShaderModuleCache {
  public:
    using Module = std::shared_ptr<const DxsoCompiledModule>;
    using Register = std::function<void(const Rc<DxvkShader>&)>;
    using Validate = std::function<void(const DxsoCompiledModule&, bool newlyCompiled)>;
    using BeforeCompile = std::function<void(const std::string&)>;

    DxsoShaderModuleCache(DxsoShaderModuleDomain domain, Register registerShader);

    // The caller must first validate/analyze the complete readable bytecode
    // extent (the IPC caller additionally uses its bounded token scanner).
    // Copies exact tokens, compiles outside the lock, validates before any
    // registration, then registers only the canonical winner. Callbacks must
    // not re-enter this cache; the registration callback runs under its lock.
    Module getOrCompile(const DxsoShaderModuleDomain& domain,
      VkShaderStageFlagBits stage, const void* bytecode, uint32_t byteCount,
      const Validate& validate, const BeforeCompile& beforeCompile = {});

    DxsoShaderModuleCacheStats stats() const;

  private:
    struct Key {
      DxvkShaderKey digest;
      std::vector<uint32_t> tokens;
      bool operator==(const Key& other) const {
        return digest.eq(other.digest) && tokens == other.tokens;
      }
    };
    struct KeyHash {
      size_t operator()(const Key& key) const { return key.digest.hash(); }
    };
    enum class State { Registering, Ready, FailedRegistration };
    struct Entry { Module module; State state = State::Registering; };

    void validateModule(const Module& module, bool fresh, const Validate& validate);
    Module readyModule(const Entry& entry) const;

    const DxsoShaderModuleDomain m_domain;
    const Register m_register;
    mutable std::mutex m_mutex;
    std::unordered_map<Key, Entry, KeyHash> m_entries;
    DxsoShaderModuleCacheStats m_stats;
  };

}
