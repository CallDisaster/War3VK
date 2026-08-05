#include "war3_shader_manager.h"
#include "../../d3d9_device.h"
#include "../core/war3_storm.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <functional>
#include <unordered_map>

namespace dxvk::war3 {
    using namespace war3shader;
    using json = nlohmann::json;

    namespace {
        std::string ToLower(std::string s) {
            std::transform(s.begin(), s.end(), s.begin(), ::tolower);
            return s;
        }

        void LoadUniformAliases(const json& obj, const std::function<void(const std::string&, uint32_t)>& setter) {
            if (!obj.is_object())
                return;
            for (auto& [key, value] : obj.items()) {
                if (value.is_number_integer()) {
                    setter(key, static_cast<uint32_t>(value.get<int>()));
                }
            }
        }

        using DefineMap = std::unordered_map<std::string, std::string>;

        std::string ToDefineValue(const json& value) {
            if (value.is_string())
                return value.get<std::string>();
            if (value.is_boolean())
                return value.get<bool>() ? "1" : "0";
            if (value.is_number())
                return std::to_string(value.get<int>());
            return "1";
        }

        void LoadDefines(const json& obj, DefineMap& out) {
            if (!obj.is_object())
                return;
            for (auto& [key, value] : obj.items()) {
                out[key] = ToDefineValue(value);
            }
        }

        void LoadDefineSection(const json& obj, DefineMap& vsOut, DefineMap& psOut) {
            if (!obj.is_object())
                return;
            const bool hasStage = obj.contains("vs") || obj.contains("ps");
            if (hasStage) {
                if (obj.contains("vs")) LoadDefines(obj["vs"], vsOut);
                if (obj.contains("ps")) LoadDefines(obj["ps"], psOut);
            } else {
                LoadDefines(obj, vsOut);
                LoadDefines(obj, psOut);
            }
        }

        std::vector<ShaderDefine> ToDefineList(const DefineMap& map) {
            std::vector<ShaderDefine> list;
            list.reserve(map.size());
            for (const auto& kv : map) {
                list.push_back({ kv.first, kv.second });
            }
            return list;
        }

        bool TryParseRenderStateName(const std::string& name, D3DRENDERSTATETYPE& outState) {
            const std::string key = ToLower(name);
            if (key == "zenable") { outState = D3DRS_ZENABLE; return true; }
            if (key == "zwriteenable") { outState = D3DRS_ZWRITEENABLE; return true; }
            if (key == "zfunc") { outState = D3DRS_ZFUNC; return true; }
            if (key == "alphablendenable") { outState = D3DRS_ALPHABLENDENABLE; return true; }
            if (key == "srcblend") { outState = D3DRS_SRCBLEND; return true; }
            if (key == "destblend") { outState = D3DRS_DESTBLEND; return true; }
            if (key == "blendop") { outState = D3DRS_BLENDOP; return true; }
            if (key == "cullmode") { outState = D3DRS_CULLMODE; return true; }
            if (key == "alphatestenable") { outState = D3DRS_ALPHATESTENABLE; return true; }
            if (key == "alphafunc") { outState = D3DRS_ALPHAFUNC; return true; }
            if (key == "alpharef") { outState = D3DRS_ALPHAREF; return true; }
            return false;
        }

        void ApplyRenderStates(const json& obj, War3Material& mat) {
            if (!obj.is_object())
                return;
            for (auto& [key, value] : obj.items()) {
                D3DRENDERSTATETYPE state;
                if (TryParseRenderStateName(key, state)) {
                    DWORD dwordValue = 0;
                    if (value.is_number())
                        dwordValue = static_cast<DWORD>(value.get<int>());
                    else if (value.is_boolean())
                        dwordValue = value.get<bool>() ? 1u : 0u;
                    mat.setRenderState(state, dwordValue);
                }
            }
        }
    } // anonymous namespace
    
    ShaderManager& ShaderManager::start() {
        static ShaderManager* instance = new ShaderManager();
        return *instance;
    }

    ShaderManager& ShaderManager::get() {
        return start();
    }

    void ShaderManager::initialize(D3D9DeviceEx* device) {
        m_device = device;
        m_globalVsConstants.reset(caps::MaxFloatConstantsVS);
        m_globalPsConstants.reset(caps::MaxSM3FloatConstantsPS);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_effectiveOverrideStageMask.store(0u, std::memory_order_release);
            m_internalTestStageOverrides.clear();
            for (auto& slot : m_internalTestStageMaterialSlots) {
                std::atomic_store_explicit(
                    &slot, std::shared_ptr<War3Material>{},
                    std::memory_order_release);
            }
            ++m_internalTestStageOverrideGeneration;
            if (m_internalTestStageOverrideGeneration == 0u)
                ++m_internalTestStageOverrideGeneration;
        }
        loadConfig("shader_packs.json");
    }

    void ShaderManager::reload() {
        loadConfig("shader_packs.json");
    }

    std::vector<ShaderPack>& ShaderManager::getPacks() {
        return m_packs;
    }

    bool ShaderManager::hasOverride(RenderStageId stage) const {
        const uint32_t bit = static_cast<uint32_t>(stage);
        const uint32_t mask = bit < 32u ? (1u << bit) : 0u;
        return mask != 0u &&
            (m_effectiveOverrideStageMask.load(std::memory_order_acquire) &
             mask) != 0u;
    }

    War3Material* ShaderManager::getMaterial(RenderStageId stage) {
        std::shared_ptr<War3Material> testMaterial;
        const uint32_t testStage = static_cast<uint32_t>(stage);
        if (testStage < m_internalTestStageMaterialSlots.size()) {
            testMaterial = std::atomic_load_explicit(
                &m_internalTestStageMaterialSlots[testStage],
                std::memory_order_acquire);
        }
        War3Material* mat = nullptr;
        if (testMaterial) {
            mat = testMaterial.get();
        } else {
            auto it = m_activeOverrides.find(stage);
            if (it != m_activeOverrides.end())
                mat = it->second;
        }
        if (mat != nullptr) {
            if (!mat->isCompiled()) {
                if (!mat->hasCompileFailure() && m_device) {
                    mat->compile(m_device, false);
                }
            }
            return mat;
        }
        return nullptr;
    }

    ShaderStageOverrideTestStatus
    ShaderManager::activateStageOverrideForTest(
        RenderStageId stage, const std::string& exactPackName) {
        std::lock_guard<std::mutex> lock(m_mutex);
        ShaderStageOverrideTestStatus status;
        if (stage != RenderStageId::Outline || exactPackName.empty())
            return status;

        const auto stageMask = [](RenderStageId value) {
            const uint32_t bit = static_cast<uint32_t>(value);
            return bit < 32u ? (1u << bit) : 0u;
        };
        const auto activeMask = [&]() {
            uint32_t mask = 0u;
            for (const auto& entry : m_activeOverrides)
                mask |= stageMask(entry.first);
            for (const auto& entry : m_internalTestStageOverrides)
                mask |= stageMask(entry.first);
            return mask;
        };
        const auto effectiveOverrides = [&]() {
            std::map<RenderStageId, War3Material*> result = m_activeOverrides;
            for (const auto& entry : m_internalTestStageOverrides) {
                result[entry.first] = entry.second.material
                    ? entry.second.material.get() : nullptr;
            }
            return result;
        };

        const auto activeBefore = effectiveOverrides();
        std::vector<bool> packEnabledBefore;
        packEnabledBefore.reserve(m_packs.size());
        for (const auto& pack : m_packs)
            packEnabledBefore.push_back(pack.enabled);
        status.activeStageMaskBefore = activeMask();
        status.worldOverrideBefore = activeBefore.find(RenderStageId::World) !=
            activeBefore.end();

        std::shared_ptr<War3Material> material;
        for (const auto& pack : m_packs) {
            if (pack.name != exactPackName)
                continue;
            status.exactPackMatched = true;
            auto candidate = pack.materials.find(stage);
            if (candidate != pack.materials.end() && candidate->second) {
                material = candidate->second;
                status.sourcePack = pack.name;
            }
            break;
        }

        status.materialExists = material != nullptr;
        auto existing = m_internalTestStageOverrides.find(stage);
        if (existing != m_internalTestStageOverrides.end()) {
            const bool sameLeaseTarget = material &&
                existing->second.generation ==
                    m_internalTestStageOverrideGeneration &&
                existing->second.sourcePack == exactPackName &&
                existing->second.material == material;
            if (sameLeaseTarget) {
                status.leaseId = existing->second.leaseId;
                status.generation = existing->second.generation;
            } else {
                status.leaseConflict = true;
            }
        }

        if (material != nullptr && !status.leaseConflict) {
            if (!material->isCompiled() && !material->hasCompileFailure() &&
                m_device != nullptr) {
                material->compile(m_device, false);
            }
            status.materialName = material->getName();
            status.materialCompiled = material->isCompiled();
            status.materialCompileFailed = material->hasCompileFailure();
            status.materialError = material->getLastError();

            // Publish only after compilation succeeds. A failed compile leaves
            // both the production cache and test overlay unchanged.
            if (status.materialCompiled && !status.materialCompileFailed &&
                existing == m_internalTestStageOverrides.end()) {
                uint64_t leaseId = m_nextInternalTestStageOverrideLeaseId++;
                if (leaseId == 0u)
                    leaseId = m_nextInternalTestStageOverrideLeaseId++;
                InternalTestStageOverride overlay;
                overlay.leaseId = leaseId;
                overlay.generation = m_internalTestStageOverrideGeneration;
                overlay.sourcePack = exactPackName;
                overlay.material = material;
                m_internalTestStageOverrides[stage] = std::move(overlay);
                const uint32_t slot = static_cast<uint32_t>(stage);
                if (slot < m_internalTestStageMaterialSlots.size()) {
                    std::atomic_store_explicit(
                        &m_internalTestStageMaterialSlots[slot], material,
                        std::memory_order_release);
                }
                rebuildEffectiveOverrideStageMask();
                status.leaseId = leaseId;
                status.generation = m_internalTestStageOverrideGeneration;
            }
        }

        status.activeStageMaskAfter = activeMask();
        const auto activeAfter = effectiveOverrides();
        status.worldOverrideAfter = activeAfter.find(RenderStageId::World) !=
            activeAfter.end();
        auto activated = m_internalTestStageOverrides.find(stage);
        status.leaseActive = status.leaseId != 0u &&
            status.generation == m_internalTestStageOverrideGeneration &&
            activated != m_internalTestStageOverrides.end() &&
            activated->second.leaseId == status.leaseId &&
            activated->second.generation == status.generation;
        status.overrideActive = status.leaseActive && material &&
            activated->second.material == material;

        auto otherBefore = activeBefore;
        auto otherAfter = activeAfter;
        otherBefore.erase(stage);
        otherAfter.erase(stage);
        status.otherStageOverridesChanged = otherBefore != otherAfter;

        std::vector<bool> packEnabledAfter;
        packEnabledAfter.reserve(m_packs.size());
        for (const auto& pack : m_packs)
            packEnabledAfter.push_back(pack.enabled);
        status.packEnabledMutated = packEnabledBefore != packEnabledAfter;
        status.stageActivationApplied = status.exactPackMatched &&
            status.materialExists && !status.leaseConflict &&
            status.overrideActive && status.materialCompiled &&
            !status.materialCompileFailed && status.leaseActive &&
            !status.otherStageOverridesChanged &&
            !status.packEnabledMutated &&
            status.worldOverrideBefore == status.worldOverrideAfter;
        return status;
    }

    bool ShaderManager::restoreStageOverrideForTest(
        RenderStageId stage, uint64_t leaseId, uint64_t generation) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (stage != RenderStageId::Outline)
            return false;
        auto entry = m_internalTestStageOverrides.find(stage);
        if (entry == m_internalTestStageOverrides.end() || leaseId == 0u ||
            generation == 0u || entry->second.leaseId != leaseId ||
            entry->second.generation != generation ||
            generation != m_internalTestStageOverrideGeneration) {
            return false;
        }
        const uint32_t slot = static_cast<uint32_t>(stage);
        m_internalTestStageOverrides.erase(entry);
        rebuildEffectiveOverrideStageMask();
        if (slot < m_internalTestStageMaterialSlots.size()) {
            std::atomic_store_explicit(
                &m_internalTestStageMaterialSlots[slot],
                std::shared_ptr<War3Material>{},
                std::memory_order_release);
        }
        return true;
    }

    bool ShaderManager::isStageOverrideTestLeaseActive(
        RenderStageId stage, uint64_t leaseId, uint64_t generation) const {
        if (stage != RenderStageId::Outline)
            return false;
        auto entry = m_internalTestStageOverrides.find(stage);
        return entry != m_internalTestStageOverrides.end() && leaseId != 0u &&
            generation != 0u && entry->second.leaseId == leaseId &&
            entry->second.generation == generation &&
            generation == m_internalTestStageOverrideGeneration;
    }

    uint32_t ShaderManager::activeOverrideMaskForTest() const {
        return m_effectiveOverrideStageMask.load(std::memory_order_acquire);
    }

    void ShaderManager::rebuildEffectiveOverrideStageMask() {
        const auto stageMask = [](RenderStageId value) {
            const uint32_t bit = static_cast<uint32_t>(value);
            return bit < 32u ? (1u << bit) : 0u;
        };
        uint32_t mask = 0u;
        for (const auto& entry : m_activeOverrides)
            mask |= stageMask(entry.first);
        for (const auto& entry : m_internalTestStageOverrides)
            mask |= stageMask(entry.first);
        m_effectiveOverrideStageMask.store(mask, std::memory_order_release);
    }

    static RenderStageId StringToStage(const std::string& s) {
        std::string lower = ToLower(s);
        
        if (lower == "shadow") return RenderStageId::Shadow;
        if (lower == "world") return RenderStageId::World;
        if (lower == "outline") return RenderStageId::Outline;
        if (lower == "ui") return RenderStageId::Ui;
        if (lower == "postprocess" || lower == "post_process" || lower == "post") return RenderStageId::PostProcess;
        if (lower == "overlay" || lower == "debug_overlay") return RenderStageId::Overlay;
        if (lower == "composite" || lower == "final") return RenderStageId::PostProcess;
        return RenderStageId::Unknown;
    }

    void ShaderManager::loadConfig(const std::string& configPath) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_packs.clear();
        m_activeOverrides.clear();
        // Fail-safe before any I/O/parse early return: remove stale production
        // bits while preserving any independently leased test overlay.
        rebuildEffectiveOverrideStageMask();
        m_globalVsConstants.reset(m_globalVsConstants.maxRegisters());
        m_globalPsConstants.reset(m_globalPsConstants.maxRegisters());
        m_missingGlobalUniforms.clear();

        std::string jsonStr;
        
        // Try filesystem first
        std::ifstream file(configPath);
        if (file.is_open()) {
            std::stringstream ss;
            ss << file.rdbuf();
            jsonStr = ss.str();
        } else {
            // Try MPQ
            std::vector<uint8_t> buffer;
            if (War3Storm::get().loadFile(configPath, buffer)) {
                jsonStr = std::string(buffer.begin(), buffer.end());
            } else {
                Logger::warn("ShaderManager: Config not found: " + configPath);
                return;
            }
        }
        
        json root;
        try {
            root = json::parse(jsonStr);
        } catch (const json::parse_error& e) {
            Logger::err(str::format("ShaderManager: JSON parse error: ", configPath, " - ", e.what()));
            return;
        }

        if (!root.is_object()) {
            Logger::err("ShaderManager: JSON root is not object: " + configPath);
            return;
        }

        DefineMap globalVsDefines;
        DefineMap globalPsDefines;
        if (root.contains("defines")) {
            LoadDefineSection(root["defines"], globalVsDefines, globalPsDefines);
        }

        if (root.contains("globals") && root["globals"].is_object()) {
            const auto& globals = root["globals"];
            if (globals.contains("vs")) {
                LoadUniformAliases(globals["vs"], [this](const std::string& name, uint32_t reg) {
                    m_globalVsConstants.setAlias(name, reg);
                });
            }
            if (globals.contains("ps")) {
                LoadUniformAliases(globals["ps"], [this](const std::string& name, uint32_t reg) {
                    m_globalPsConstants.setAlias(name, reg);
                });
            }
        }

        if (root.contains("packs") && root["packs"].is_array()) {
            for (const auto& p : root["packs"]) {
                ShaderPack pack;
                pack.name = p.value("name", "Unnamed");
                pack.description = p.value("description", "");
                pack.enabled = p.value("enabled", false);

                DefineMap packVsDefines = globalVsDefines;
                DefineMap packPsDefines = globalPsDefines;
                if (p.contains("defines")) {
                    LoadDefineSection(p["defines"], packVsDefines, packPsDefines);
                }

                if (p.contains("passes") && p["passes"].is_object()) {
                    for (auto& [stageName, passData] : p["passes"].items()) {
                        RenderStageId stage = StringToStage(stageName);
                        if (stage != RenderStageId::Unknown) {
                            std::string matName = pack.name + "_" + stageName;
                            auto mat = std::make_shared<War3Material>(matName);

                            mat->setVertexShaderPath(passData.value("vs", ""));
                            mat->setPixelShaderPath(passData.value("ps", ""));
                            
                            if (passData.contains("vs_entry")) {
                                mat->setVertexShaderPath(passData.value("vs", ""), passData.value("vs_entry", "main"));
                            }
                            if (passData.contains("ps_entry")) {
                                mat->setPixelShaderPath(passData.value("ps", ""), passData.value("ps_entry", "main"));
                            }

                            DefineMap passVsDefines = packVsDefines;
                            DefineMap passPsDefines = packPsDefines;
                            if (passData.contains("defines")) {
                                LoadDefineSection(passData["defines"], passVsDefines, passPsDefines);
                            }
                            mat->setVertexShaderDefines(ToDefineList(passVsDefines));
                            mat->setPixelShaderDefines(ToDefineList(passPsDefines));

                            if (passData.contains("uniforms") && passData["uniforms"].is_object()) {
                                const auto& uniforms = passData["uniforms"];
                                if (uniforms.contains("vs")) {
                                    LoadUniformAliases(uniforms["vs"], [mat](const std::string& name, uint32_t reg) {
                                        mat->setVertexRegisterAlias(name, reg);
                                    });
                                }
                                if (uniforms.contains("ps")) {
                                    LoadUniformAliases(uniforms["ps"], [mat](const std::string& name, uint32_t reg) {
                                        mat->setPixelRegisterAlias(name, reg);
                                    });
                                }
                            }

                            if (passData.contains("render_states")) {
                                ApplyRenderStates(passData["render_states"], *mat);
                            }
                            
                            pack.materials[stage] = mat;
                        }
                    }
                }
                m_packs.push_back(pack);
            }
        }

        rebuildCache();
        Logger::info(str::format("ShaderManager: Loaded ", m_packs.size(), " packs from ", configPath));
    }

    void ShaderManager::rebuildCache() {
        m_activeOverrides.clear();
        for (const auto& pack : m_packs) {
            if (pack.enabled) {
                for (const auto& kv : pack.materials) {
                    m_activeOverrides[kv.first] = kv.second.get();
                }
            }
        }
        rebuildEffectiveOverrideStageMask();
    }

    void ShaderManager::setGlobalVertexShaderConstantF(UINT startRegister, const float* data, UINT vec4Count) {
        m_globalVsConstants.setConstantF(startRegister, data, vec4Count);
    }

    void ShaderManager::setGlobalPixelShaderConstantF(UINT startRegister, const float* data, UINT vec4Count) {
        m_globalPsConstants.setConstantF(startRegister, data, vec4Count);
    }

    void ShaderManager::setGlobalFloat4(const std::string& name, const Vector4& v) {
        const bool vsOk = m_globalVsConstants.setFloat4ByName(name, v);
        const bool psOk = m_globalPsConstants.setFloat4ByName(name, v);
        if (!vsOk && !psOk && m_missingGlobalUniforms.insert(name).second) {
            Logger::warn("ShaderManager: Global Uniform missing register: " + name);
        }
    }

    void ShaderManager::setGlobalMatrix(const std::string& name, const Matrix4& m) {
        const bool vsOk = m_globalVsConstants.setMatrixByName(name, m);
        const bool psOk = m_globalPsConstants.setMatrixByName(name, m);
        if (!vsOk && !psOk && m_missingGlobalUniforms.insert(name).second) {
            Logger::warn("ShaderManager: Global Uniform missing register: " + name);
        }
    }

    void ShaderManager::setGlobalVertexRegisterAlias(const std::string& name, uint32_t reg) {
        m_globalVsConstants.setAlias(name, reg);
    }

    void ShaderManager::setGlobalPixelRegisterAlias(const std::string& name, uint32_t reg) {
        m_globalPsConstants.setAlias(name, reg);
    }

    void ShaderManager::applyGlobalUniforms(D3D9DeviceEx* device) const {
        if (!device)
            return;
        auto applyStore = [device](const ShaderConstantStore& store, bool isVertex) {
            if (!store.hasAny())
                return;
            std::vector<ShaderConstantRange> ranges;
            store.collectRanges(ranges);
            for (const auto& range : ranges) {
                const float* data = store.data(range.start);
                if (!data)
                    continue;
                if (isVertex) {
                    device->SetVertexShaderConstantF(range.start, data, range.count);
                } else {
                    device->SetPixelShaderConstantF(range.start, data, range.count);
                }
            }
        };
        applyStore(m_globalVsConstants, true);
        applyStore(m_globalPsConstants, false);
    }
    
    bool ShaderManager::loadResource(const std::string& path, std::vector<uint8_t>& outBuffer) {
        std::ifstream file(path, std::ios::binary);
        if (file.is_open()) {
            file.seekg(0, std::ios::end);
            size_t size = file.tellg();
            if (size > 0) {
                outBuffer.resize(size);
                file.seekg(0, std::ios::beg);
                file.read(reinterpret_cast<char*>(outBuffer.data()), size);
                return true;
            }
        }
        
        if (War3Storm::get().exists(path)) {
            return War3Storm::get().loadFile(path, outBuffer);
        }
        
        return false;
    }

}
