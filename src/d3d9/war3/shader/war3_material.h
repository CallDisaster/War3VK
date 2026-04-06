#pragma once

#include <d3d9.h>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include "../../d3d9_device.h"
#include "../../d3d9_caps.h"
#include "../../../util/util_vector.h"
#include "../../../util/util_matrix.h"

namespace dxvk::war3 {

    enum class MaterialStage {
        Main = 0,
        Shadow = 1,
        Outline = 2,
        UI = 3,
        Count
    };

    struct ShaderBytecode {
        std::vector<uint8_t> data;
        std::string entryPoint;
        std::string profile;
    };

    struct ShaderConstantRange {
        uint32_t start = 0;
        uint32_t count = 0;
    };

    struct ShaderDefine {
        std::string name;
        std::string value;
    };

    class ShaderConstantStore {
    public:
        ShaderConstantStore() = default;
        explicit ShaderConstantStore(uint32_t maxRegisters);

        void reset(uint32_t maxRegisters);
        void clear();

        void setAlias(const std::string& name, uint32_t reg);
        bool hasAlias(const std::string& name) const;

        bool setConstantF(UINT startRegister, const float* data, UINT vec4Count);
        bool setFloat4ByName(const std::string& name, const Vector4& v);
        bool setMatrixByName(const std::string& name, const Matrix4& m);

        void collectRanges(std::vector<ShaderConstantRange>& out) const;
        const float* data(uint32_t startRegister) const;
        uint32_t maxRegisters() const { return m_maxRegisters; }
        bool hasAny() const;

    private:
        uint32_t m_maxRegisters = 0;
        std::vector<float> m_data;
        std::vector<uint8_t> m_setMask;
        std::unordered_map<std::string, uint32_t> m_aliases;
    };

    class War3Material {
    public:
        War3Material(const std::string& name);
        ~War3Material();

        bool compile(D3D9DeviceEx* device, bool force = false);
        void apply(D3D9DeviceEx* device);

        // 配置
        void setVertexShaderPath(const std::string& path, const std::string& entry = "main_vs");
        void setPixelShaderPath(const std::string& path, const std::string& entry = "main_ps");
        void setVertexShaderDefines(const std::vector<ShaderDefine>& defines);
        void setPixelShaderDefines(const std::vector<ShaderDefine>& defines);
        
        void setRenderState(D3DRENDERSTATETYPE state, DWORD value);
        void setTexture(uint32_t stage, const std::string& path); // 纹理加载占位
        void setVertexShaderConstantF(UINT startRegister, const float* data, UINT vec4Count);
        void setPixelShaderConstantF(UINT startRegister, const float* data, UINT vec4Count);
        void setFloat4(const std::string& name, const Vector4& v);
        void setMatrix(const std::string& name, const Matrix4& m);
        void setVertexRegisterAlias(const std::string& name, uint32_t reg);
        void setPixelRegisterAlias(const std::string& name, uint32_t reg);

        bool isCompiled() const { return m_compiled; }
        bool hasCompileFailure() const { return m_compileFailed; }
        const std::string& getLastError() const { return m_lastError; }
        const std::string& getName() const { return m_name; }

        IDirect3DVertexShader9* getVertexShader() const { return m_vs.ptr(); }
        IDirect3DPixelShader9* getPixelShader() const { return m_ps.ptr(); }
        const std::map<D3DRENDERSTATETYPE, DWORD>& getRenderStates() const { return m_renderStates; }
        const ShaderConstantStore& getVertexConstants() const { return m_vsConstants; }
        const ShaderConstantStore& getPixelConstants() const { return m_psConstants; }

    private:
        std::string m_name;
        bool m_compiled = false;
        bool m_compileFailed = false;
        std::string m_lastError;

        // 源路径
        std::string m_vsPath;
        std::string m_vsEntry;
        std::string m_psPath;
        std::string m_psEntry;
        std::vector<ShaderDefine> m_vsDefines;
        std::vector<ShaderDefine> m_psDefines;

        // D3D 对象
        Com<IDirect3DVertexShader9> m_vs;
        Com<IDirect3DPixelShader9> m_ps;

        // 状态
        std::map<D3DRENDERSTATETYPE, DWORD> m_renderStates;
        ShaderConstantStore m_vsConstants;
        ShaderConstantStore m_psConstants;
        std::unordered_set<std::string> m_missingUniforms;

        void applyConstantStore(D3D9DeviceEx* device, const ShaderConstantStore& store, bool isVertex) const;

        // 编译单个着色器
        template <typename T>
        bool compileShader(D3D9DeviceEx* device,
                           const std::string& path,
                           const std::string& entry,
                           const std::string& profile,
                           const std::vector<ShaderDefine>& defines,
                           Com<T>& outShader,
                           std::string* outError);
    };

    using War3MaterialPtr = std::shared_ptr<War3Material>;

}
