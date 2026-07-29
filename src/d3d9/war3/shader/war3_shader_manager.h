#pragma once

#include <vector>
#include <array>
#include <string>
#include <map>
#include <atomic>
#include <mutex>
#include <memory>
#include <unordered_set>
#include <d3d9.h>
#include "../../war3_shader_api.h"
#include "war3_material.h"

namespace dxvk {
    class D3D9DeviceEx;
}

namespace dxvk::war3 {

    struct ShaderPack {
        std::string name;
        std::string description;
        bool enabled = false;
        
        // 阶段到材质的映射
        std::map<war3shader::RenderStageId, std::shared_ptr<War3Material>> materials;
    };

    struct ShaderStageOverrideTestStatus {
        bool exactPackMatched = false;
        bool materialExists = false;
        bool overrideActive = false;
        bool materialCompiled = false;
        bool materialCompileFailed = false;
        bool stageActivationApplied = false;
        bool leaseActive = false;
        bool leaseConflict = false;
        bool otherStageOverridesChanged = false;
        bool packEnabledMutated = false;
        bool worldOverrideBefore = false;
        bool worldOverrideAfter = false;
        uint32_t activeStageMaskBefore = 0u;
        uint32_t activeStageMaskAfter = 0u;
        uint64_t leaseId = 0u;
        uint64_t generation = 0u;
        std::string sourcePack;
        std::string materialName;
        std::string materialError;
    };

    class ShaderManager {
    public:
        static ShaderManager& start();
        static ShaderManager& get();

        // 设备初始化（用于后续编译）
        void initialize(D3D9DeviceEx* device);
        
        // 加载配置
        void loadConfig(const std::string& configPath);
        
        // 重新加载全部配置
        void reload();

        // 获取全部 ShaderPack（UI 使用）
        std::vector<ShaderPack>& getPacks();

        // 是否存在阶段覆盖
        bool hasOverride(war3shader::RenderStageId stage) const;
        
        // 获取阶段覆盖材质
        War3Material* getMaterial(war3shader::RenderStageId stage);

        // Internal-test only: expose one already-loaded material as one stage
        // override without enabling its entire ShaderPack.
        ShaderStageOverrideTestStatus activateStageOverrideForTest(
            war3shader::RenderStageId stage,
            const std::string& exactPackName);
        bool restoreStageOverrideForTest(
            war3shader::RenderStageId stage,
            uint64_t leaseId,
            uint64_t generation);
        bool isStageOverrideTestLeaseActive(
            war3shader::RenderStageId stage,
            uint64_t leaseId,
            uint64_t generation) const;
        uint32_t activeOverrideMaskForTest() const;

        void setGlobalVertexShaderConstantF(UINT startRegister, const float* data, UINT vec4Count);
        void setGlobalPixelShaderConstantF(UINT startRegister, const float* data, UINT vec4Count);
        void setGlobalFloat4(const std::string& name, const Vector4& v);
        void setGlobalMatrix(const std::string& name, const Matrix4& m);
        void setGlobalVertexRegisterAlias(const std::string& name, uint32_t reg);
        void setGlobalPixelRegisterAlias(const std::string& name, uint32_t reg);
        void applyGlobalUniforms(D3D9DeviceEx* device) const;
        const ShaderConstantStore& getGlobalVertexConstants() const { return m_globalVsConstants; }
        const ShaderConstantStore& getGlobalPixelConstants() const { return m_globalPsConstants; }
        
        // 资源加载辅助（文件系统/MPQ）
        static bool loadResource(const std::string& path, std::vector<uint8_t>& outBuffer);
        
        // 重建内部缓存
        void rebuildCache();

    private:
        ShaderManager() = default;
        
        D3D9DeviceEx* m_device = nullptr;
        std::vector<ShaderPack> m_packs;
        std::mutex m_mutex;
        
        // 运行时覆盖缓存（性能）
        std::map<war3shader::RenderStageId, War3Material*> m_activeOverrides;

        struct InternalTestStageOverride {
            uint64_t leaseId = 0u;
            uint64_t generation = 0u;
            std::string sourcePack;
            std::shared_ptr<War3Material> material;
        };
        // Test overlays never mutate pack.enabled or the production cache.
        // All reads and mutations are confined to the War3/D3D9 main thread.
        std::map<war3shader::RenderStageId, InternalTestStageOverride>
            m_internalTestStageOverrides;
        std::array<std::shared_ptr<War3Material>, 8>
            m_internalTestStageMaterialSlots = {};
        uint64_t m_internalTestStageOverrideGeneration = 1u;
        uint64_t m_nextInternalTestStageOverrideLeaseId = 1u;
        std::atomic<uint32_t> m_effectiveOverrideStageMask{0u};

        void rebuildEffectiveOverrideStageMask();

        ShaderConstantStore m_globalVsConstants;
        ShaderConstantStore m_globalPsConstants;
        mutable std::unordered_set<std::string> m_missingGlobalUniforms;
    };

}
