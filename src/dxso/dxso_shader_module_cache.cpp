#include "dxso_shader_module_cache.h"

#include <cstring>

namespace dxvk {
  namespace {
    bool sameLayout(const D3D9ConstantLayout& a, const D3D9ConstantLayout& b) {
      return a.floatCount == b.floatCount && a.intCount == b.intCount
        && a.boolCount == b.boolCount && a.bitmaskCount == b.bitmaskCount;
    }
  }

  bool DxsoShaderModuleDomain::matches(const DxsoShaderModuleDomain& other) const {
    const auto& a = moduleInfo.options;
    const auto& b = other.moduleInfo.options;
    return deviceIdentity != nullptr && deviceIdentity == other.deviceIdentity
      && a.strictConstantCopies == b.strictConstantCopies
      && a.d3d9FloatEmulation == b.d3d9FloatEmulation
      && a.strictPow == b.strictPow
      && a.invariantPosition == b.invariantPosition
      && a.forceSamplerTypeSpecConstants == b.forceSamplerTypeSpecConstants
      && a.forceSampleRateShading == b.forceSampleRateShading
      && a.vertexFloatConstantBufferAsSSBO == b.vertexFloatConstantBufferAsSSBO
      && a.robustness2Supported == b.robustness2Supported
      && a.sincosEmulation == b.sincosEmulation
      && sameLayout(vertexLayout, other.vertexLayout)
      && sameLayout(pixelLayout, other.pixelLayout);
  }

  DxsoShaderModuleCache::DxsoShaderModuleCache(
      DxsoShaderModuleDomain domain, Register registerShader)
  : m_domain(std::move(domain)), m_register(std::move(registerShader)) {
    if (!m_domain.deviceIdentity || !m_register)
      throw DxvkError("DXSO module cache requires a device domain and registrar");
  }

  DxsoShaderModuleCache::Module DxsoShaderModuleCache::readyModule(const Entry& entry) const {
    if (entry.state != State::Ready)
      throw DxvkError("DXSO shader registration previously failed; same-key retry refused");
    return entry.module;
  }

  void DxsoShaderModuleCache::validateModule(
      const Module& module, bool fresh, const Validate& validate) {
    try {
      validate(*module, fresh);
    } catch (...) {
      std::lock_guard<std::mutex> lock(m_mutex);
      ++m_stats.validationFailures;
      throw;
    }
  }

  DxsoShaderModuleCache::Module DxsoShaderModuleCache::getOrCompile(
      const DxsoShaderModuleDomain& domain, VkShaderStageFlagBits stage,
      const void* bytecode, uint32_t byteCount, const Validate& validate,
      const BeforeCompile& beforeCompile) {
    if (!m_domain.matches(domain))
      throw DxvkError("DXSO shader cache compilation domain mismatch");
    if (!validate || !bytecode || byteCount < 8 || byteCount % sizeof(uint32_t)
        || (stage != VK_SHADER_STAGE_VERTEX_BIT && stage != VK_SHADER_STAGE_FRAGMENT_BIT))
      throw DxvkError("DXSO shader cache invalid analyzed input");

    Key key;
    key.tokens.resize(byteCount / sizeof(uint32_t));
    std::memcpy(key.tokens.data(), bytecode, byteCount);
    key.digest = DxvkShaderKey(stage, Sha1Hash::compute(key.tokens.data(), byteCount));

    Module existing;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      ++m_stats.requests;
      const auto found = m_entries.find(key);
      if (found != m_entries.end()) {
        existing = readyModule(found->second);
        ++m_stats.hits;
      }
    }
    if (existing) {
      validateModule(existing, false, validate);
      return existing;
    }

    DxsoReader reader(reinterpret_cast<const char*>(key.tokens.data()));
    DxsoModule source(reader);
    if (source.info().shaderStage() != stage)
      throw DxvkError("DXSO shader cache stage mismatch");
    const auto analysis = source.analyze();
    if (analysis.bytecodeByteLength != byteCount)
      throw DxvkError("DXSO shader cache analyzed extent mismatch");
    const auto name = key.digest.toString();
    if (beforeCompile)
      beforeCompile(name);
    auto candidate = std::make_shared<DxsoCompiledModule>();
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      ++m_stats.compileAttempts;
    }
    candidate->shader = source.compile(m_domain.moduleInfo, name, analysis,
      stage == VK_SHADER_STAGE_VERTEX_BIT ? m_domain.vertexLayout : m_domain.pixelLayout);
    if (candidate->shader == nullptr)
      throw DxvkError("DXSO compiler returned no shader");
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      ++m_stats.compilesSucceeded;
    }
    candidate->isgn = source.isgn();
    candidate->usedSamplers = source.usedSamplers();
    candidate->usedRTs = source.usedRTs();
    candidate->textureTypes = source.textureTypes();
    candidate->info = source.info();
    candidate->meta = source.meta();
    candidate->constants = source.constants();
    candidate->maxDefinedFloat = source.maxDefinedFloatConstant();
    candidate->maxDefinedInt = source.maxDefinedIntConstant();
    candidate->maxDefinedBool = source.maxDefinedBoolConstant();
    validateModule(candidate, true, validate);

    std::unique_lock<std::mutex> lock(m_mutex);
    const auto found = m_entries.find(key);
    if (found != m_entries.end()) {
      existing = readyModule(found->second);
      ++m_stats.raceLosers;
      ++m_stats.hits;
      lock.unlock();
      validateModule(existing, false, validate);
      return existing;
    }
    // Allocate the canonical entry BEFORE potentially irreversible registration.
    // On an exception the retained failed entry prevents a duplicate/partial
    // retry; no rollback of the DXVK pipeline manager is claimed.
    auto inserted = m_entries.emplace(std::move(key), Entry{candidate, State::Registering});
    auto& entry = inserted.first->second;
    try {
      m_register(candidate->shader);
      entry.state = State::Ready;
      ++m_stats.registrations;
    } catch (...) {
      entry.state = State::FailedRegistration;
      ++m_stats.registrationFailures;
      throw;
    }
    return entry.module;
  }

  DxsoShaderModuleCacheStats DxsoShaderModuleCache::stats() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto result = m_stats;
    for (const auto& item : m_entries) {
      result.readyEntries += item.second.state == State::Ready;
      result.failedEntries += item.second.state == State::FailedRegistration;
      result.keyBytes += item.first.tokens.size() * sizeof(uint32_t);
      result.keyCapacityBytes += item.first.tokens.capacity() * sizeof(uint32_t);
      result.constantBytes += item.second.module->constants.size() * sizeof(DxsoDefinedConstant);
      result.constantCapacityBytes += item.second.module->constants.capacity() * sizeof(DxsoDefinedConstant);
    }
    return result;
  }
}
