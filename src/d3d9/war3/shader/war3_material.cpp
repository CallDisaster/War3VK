#include "war3_material.h"
#include "war3_shader_manager.h" // 资源加载辅助（未来可拆分）
#include "../../d3d9_util.h"
// #include "../core/war3_file_manager.h" // 已改用 ShaderManager::loadResource

#include <d3dcompiler.h>
#include "../../../dxso/dxso_module.h"
#include "../../../dxso/dxso_modinfo.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace dxvk::war3 {

    namespace {
        bool IsAbsolutePath(const std::string& path) {
            if (path.size() >= 2 && path[1] == ':')
                return true;
            if (!path.empty() && (path[0] == '\\' || path[0] == '/'))
                return true;
            return false;
        }

        std::string GetDirectory(const std::string& path) {
            const size_t pos = path.find_last_of("\\/");
            if (pos == std::string::npos)
                return std::string();
            return path.substr(0, pos + 1);
        }

        std::string JoinPath(const std::string& baseDir, const std::string& file) {
            if (baseDir.empty())
                return file;
            if (IsAbsolutePath(file))
                return file;
            return baseDir + file;
        }

        class ShaderIncludeHandler final : public ID3DInclude {
        public:
            explicit ShaderIncludeHandler(std::string baseDir)
                : m_baseDir(std::move(baseDir)) {}

            HRESULT STDMETHODCALLTYPE Open(D3D_INCLUDE_TYPE, LPCSTR fileName, LPCVOID, const void** data, UINT* bytes) override {
                if (!fileName || !data || !bytes)
                    return E_FAIL;
                std::string path = JoinPath(m_baseDir, fileName);
                std::vector<uint8_t> buffer;
                if (!ShaderManager::loadResource(path, buffer))
                    return E_FAIL;
                if (buffer.empty()) {
                    *data = nullptr;
                    *bytes = 0;
                    return S_OK;
                }
                uint8_t* mem = new uint8_t[buffer.size()];
                std::memcpy(mem, buffer.data(), buffer.size());
                *data = mem;
                *bytes = static_cast<UINT>(buffer.size());
                return S_OK;
            }

            HRESULT STDMETHODCALLTYPE Close(const void* data) override {
                delete[] reinterpret_cast<const uint8_t*>(data);
                return S_OK;
            }

        private:
            std::string m_baseDir;
        };
    }

    ShaderConstantStore::ShaderConstantStore(uint32_t maxRegisters) {
        reset(maxRegisters);
    }

    void ShaderConstantStore::reset(uint32_t maxRegisters) {
        m_maxRegisters = maxRegisters;
        m_data.assign(m_maxRegisters * 4, 0.0f);
        m_setMask.assign(m_maxRegisters, 0u);
        m_aliases.clear();
    }

    void ShaderConstantStore::clear() {
        std::fill(m_data.begin(), m_data.end(), 0.0f);
        std::fill(m_setMask.begin(), m_setMask.end(), 0u);
    }

    void ShaderConstantStore::setAlias(const std::string& name, uint32_t reg) {
        m_aliases[name] = reg;
    }

    bool ShaderConstantStore::hasAlias(const std::string& name) const {
        return m_aliases.find(name) != m_aliases.end();
    }

    bool ShaderConstantStore::setConstantF(UINT startRegister, const float* data, UINT vec4Count) {
        if (data == nullptr || vec4Count == 0)
            return false;
        if (startRegister >= m_maxRegisters)
            return false;

        const uint32_t end = std::min<uint32_t>(startRegister + vec4Count, m_maxRegisters);
        for (uint32_t reg = startRegister; reg < end; ++reg) {
            const uint32_t srcOffset = (reg - startRegister) * 4;
            const uint32_t dstOffset = reg * 4;
            std::copy_n(data + srcOffset, 4, m_data.data() + dstOffset);
            m_setMask[reg] = 1u;
        }
        return true;
    }

    bool ShaderConstantStore::setFloat4ByName(const std::string& name, const Vector4& v) {
        auto it = m_aliases.find(name);
        if (it == m_aliases.end())
            return false;
        return setConstantF(it->second, v.data, 1);
    }

    bool ShaderConstantStore::setMatrixByName(const std::string& name, const Matrix4& m) {
        auto it = m_aliases.find(name);
        if (it == m_aliases.end())
            return false;
        return setConstantF(it->second, &m.data[0].x, 4);
    }

    void ShaderConstantStore::collectRanges(std::vector<ShaderConstantRange>& out) const {
        out.clear();
        if (m_setMask.empty())
            return;
        uint32_t i = 0;
        while (i < m_setMask.size()) {
            if (!m_setMask[i]) {
                ++i;
                continue;
            }
            uint32_t start = i;
            while (i < m_setMask.size() && m_setMask[i])
                ++i;
            out.push_back({ start, i - start });
        }
    }

    const float* ShaderConstantStore::data(uint32_t startRegister) const {
        if (startRegister >= m_maxRegisters)
            return nullptr;
        return m_data.data() + startRegister * 4;
    }

    bool ShaderConstantStore::hasAny() const {
        for (auto v : m_setMask) {
            if (v)
                return true;
        }
        return false;
    }

    War3Material::War3Material(const std::string& name) 
        : m_name(name)
        , m_vsConstants(caps::MaxFloatConstantsVS)
        , m_psConstants(caps::MaxSM3FloatConstantsPS) {
    }

    War3Material::~War3Material() {
    }

    void War3Material::setVertexShaderPath(const std::string& path, const std::string& entry) {
        m_vsPath = path;
        m_vsEntry = entry;
        m_compiled = false;
        m_compileFailed = false;
        m_lastError.clear();
    }

    void War3Material::setPixelShaderPath(const std::string& path, const std::string& entry) {
        m_psPath = path;
        m_psEntry = entry;
        m_compiled = false;
        m_compileFailed = false;
        m_lastError.clear();
    }

    void War3Material::setVertexShaderDefines(const std::vector<ShaderDefine>& defines) {
        m_vsDefines = defines;
        m_compiled = false;
        m_compileFailed = false;
        m_lastError.clear();
    }

    void War3Material::setPixelShaderDefines(const std::vector<ShaderDefine>& defines) {
        m_psDefines = defines;
        m_compiled = false;
        m_compileFailed = false;
        m_lastError.clear();
    }

    void War3Material::setRenderState(D3DRENDERSTATETYPE state, DWORD value) {
        m_renderStates[state] = value;
    }

    void War3Material::setVertexShaderConstantF(UINT startRegister, const float* data, UINT vec4Count) {
        m_vsConstants.setConstantF(startRegister, data, vec4Count);
    }

    void War3Material::setPixelShaderConstantF(UINT startRegister, const float* data, UINT vec4Count) {
        m_psConstants.setConstantF(startRegister, data, vec4Count);
    }

    void War3Material::setFloat4(const std::string& name, const Vector4& v) {
        const bool vsOk = m_vsConstants.setFloat4ByName(name, v);
        const bool psOk = m_psConstants.setFloat4ByName(name, v);
        if (!vsOk && !psOk && m_missingUniforms.insert(name).second) {
            Logger::warn("War3Material: Uniform 未绑定寄存器: " + name);
        }
    }

    void War3Material::setMatrix(const std::string& name, const Matrix4& m) {
        const bool vsOk = m_vsConstants.setMatrixByName(name, m);
        const bool psOk = m_psConstants.setMatrixByName(name, m);
        if (!vsOk && !psOk && m_missingUniforms.insert(name).second) {
            Logger::warn("War3Material: Uniform 未绑定寄存器: " + name);
        }
    }

    void War3Material::setVertexRegisterAlias(const std::string& name, uint32_t reg) {
        m_vsConstants.setAlias(name, reg);
    }

    void War3Material::setPixelRegisterAlias(const std::string& name, uint32_t reg) {
        m_psConstants.setAlias(name, reg);
    }

    bool War3Material::compile(D3D9DeviceEx* device, bool force) {
        Logger::info(str::format("War3Material::compile called for ", m_name, ", force=", force));
        if (!device) {
            Logger::err("War3Material::compile: device is NULL!");
            return false;
        }
        if (!force && m_compileFailed) {
            Logger::info("War3Material::compile: skipping (already failed and force=false)");
            return false;
        }

        bool vsOk = true;
        bool psOk = true;
        m_lastError.clear();
        m_compileFailed = false;

        if (!m_vsPath.empty()) {
            std::string vsError;
            vsOk = compileShader<IDirect3DVertexShader9>(device, m_vsPath, m_vsEntry, "vs_3_0", m_vsDefines, m_vs, &vsError);
            if (!vsOk)
                m_lastError = "VS: " + vsError;
        }

        if (!m_psPath.empty()) {
            std::string psError;
            psOk = compileShader<IDirect3DPixelShader9>(device, m_psPath, m_psEntry, "ps_3_0", m_psDefines, m_ps, &psError);
            if (!psOk) {
                if (!m_lastError.empty())
                    m_lastError.append(" | ");
                m_lastError.append("PS: ").append(psError);
            }
        }

        if (!vsOk || !psOk) {
            m_compiled = false;
            m_compileFailed = true;
            return false;
        }

        m_compiled = true;
        return true;
    }

    void War3Material::apply(D3D9DeviceEx* device) {
        if (!m_compiled) return;

        if (m_vs != nullptr) device->SetVertexShader(m_vs.ptr());
        if (m_ps != nullptr) device->SetPixelShader(m_ps.ptr());

        for (auto& state : m_renderStates) {
            device->SetRenderState(state.first, state.second);
        }

        ShaderManager::get().applyGlobalUniforms(device);
        applyConstantStore(device, m_vsConstants, true);
        applyConstantStore(device, m_psConstants, false);
    }

    void War3Material::applyConstantStore(D3D9DeviceEx* device, const ShaderConstantStore& store, bool isVertex) const {
        if (!device || !store.hasAny())
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
    }

    // 动态编译辅助
    // 依赖 d3dcompiler_47.dll
    typedef HRESULT (WINAPI *pD3DCompile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    typedef HRESULT (WINAPI *pD3DGetBlobPart)(LPCVOID, SIZE_T, D3D_BLOB_PART, UINT, ID3DBlob**);

    namespace {
        constexpr uint32_t MakeFourCC(char a, char b, char c, char d) {
            return uint32_t(uint8_t(a)) |
                   (uint32_t(uint8_t(b)) << 8) |
                   (uint32_t(uint8_t(c)) << 16) |
                   (uint32_t(uint8_t(d)) << 24);
        }

        constexpr uint32_t kDxbcMagic = MakeFourCC('D', 'X', 'B', 'C');
        constexpr uint32_t kShdrMagic = MakeFourCC('S', 'H', 'D', 'R');
        constexpr uint32_t kShexMagic = MakeFourCC('S', 'H', 'E', 'X');

        bool IsDxsoToken(uint32_t token) {
            const uint32_t high = token & 0xFFFF0000u;
            return high == 0xFFFE0000u || high == 0xFFFF0000u;
        }

        bool TryGetDxsoTokenFromProfile(const std::string& profile, uint32_t& outToken) {
            if (profile.empty())
                return false;
            unsigned major = 0;
            unsigned minor = 0;
            if (std::sscanf(profile.c_str(), "vs_%u_%u", &major, &minor) == 2) {
                outToken = 0xFFFE0000u | (major << 8) | minor;
                return true;
            }
            if (std::sscanf(profile.c_str(), "ps_%u_%u", &major, &minor) == 2) {
                outToken = 0xFFFF0000u | (major << 8) | minor;
                return true;
            }
            return false;
        }

        bool ReadU32(const uint8_t* data, size_t size, size_t offset, uint32_t& outValue) {
            if (!data || offset + sizeof(uint32_t) > size)
                return false;
            std::memcpy(&outValue, data + offset, sizeof(uint32_t));
            return true;
        }

        bool ExtractDxsoFromDxbc(const void* data, size_t size, std::vector<uint8_t>& outDxso, std::string* outError) {
            auto setError = [outError](const std::string& message) {
                if (outError)
                    *outError = message;
            };

            if (!data || size < 32u) {
                setError("DXBC 数据过小，无法解析");
                return false;
            }

            const auto* bytes = reinterpret_cast<const uint8_t*>(data);
            uint32_t magic = 0;
            if (!ReadU32(bytes, size, 0, magic) || magic != kDxbcMagic) {
                setError("DXBC Magic 不匹配");
                return false;
            }

            uint32_t fileSize = 0;
            uint32_t chunkCount = 0;
            if (!ReadU32(bytes, size, 24, fileSize) || !ReadU32(bytes, size, 28, chunkCount)) {
                setError("DXBC 头部读取失败");
                return false;
            }
            if (fileSize > size || chunkCount == 0 || chunkCount > 256) {
                setError("DXBC 头部参数异常");
                return false;
            }

            const size_t chunkTableOffset = 32u;
            const size_t chunkTableSize = size_t(chunkCount) * sizeof(uint32_t);
            if (chunkTableOffset + chunkTableSize > size) {
                setError("DXBC 块表越界");
                return false;
            }

            auto tryExtractDxso = [&](const uint8_t* chunkData, uint32_t chunkSize) -> bool {
                if (!chunkData || chunkSize < sizeof(uint32_t))
                    return false;
                for (uint32_t offset = 0; offset + sizeof(uint32_t) <= chunkSize; offset += sizeof(uint32_t)) {
                    uint32_t token = 0;
                    std::memcpy(&token, chunkData + offset, sizeof(uint32_t));
                    if (IsDxsoToken(token)) {
                        outDxso.assign(chunkData + offset, chunkData + chunkSize);
                        return !outDxso.empty();
                    }
                }
                return false;
            };

            for (uint32_t i = 0; i < chunkCount; ++i) {
                uint32_t chunkOffset = 0;
                if (!ReadU32(bytes, size, chunkTableOffset + i * sizeof(uint32_t), chunkOffset))
                    continue;
                if (chunkOffset + 8u > size)
                    continue;

                uint32_t tag = 0;
                uint32_t chunkSize = 0;
                if (!ReadU32(bytes, size, chunkOffset, tag) || !ReadU32(bytes, size, chunkOffset + 4u, chunkSize))
                    continue;
                if (chunkOffset + 8u + chunkSize > size)
                    continue;

                if (tag == kShdrMagic || tag == kShexMagic) {
                    const uint8_t* chunkData = bytes + chunkOffset + 8u;
                    if (tryExtractDxso(chunkData, chunkSize))
                        return true;
                }
            }

            setError("DXBC 中未找到 SHDR/SHEX 块");
            return false;
        }

        std::string DiagnoseDxsoFailure(D3D9DeviceEx* device, const void* shaderData, size_t shaderSize, const std::string& path, bool isVertex) {
            if (!device || !shaderData || shaderSize < sizeof(uint32_t))
                return std::string();
            const uint32_t token0 = reinterpret_cast<const uint32_t*>(shaderData)[0];
            const uint32_t headerMask = token0 & 0xFFFF0000u;
            if (headerMask != 0xFFFF0000u && headerMask != 0xFFFE0000u) {
                return str::format("DXSO Header 无效 token0=0x", std::hex, token0, std::dec);
            }
            try {
                DxsoModuleInfo moduleInfo;
                moduleInfo.options = DxsoOptions(device, *device->GetOptions());
                DxsoReader reader(reinterpret_cast<const char*>(shaderData));
                DxsoModule module(reader);
                DxsoAnalysisInfo info = module.analyze();
                const D3D9ConstantLayout& layout = isVertex ? device->GetVertexConstantLayout() : device->GetPixelConstantLayout();
                module.compile(moduleInfo, path, info, layout);
            } catch (const DxvkError& e) {
                return e.message();
            }
            return std::string();
        }
    }

    template <typename T>
    bool War3Material::compileShader(D3D9DeviceEx* device,
                                     const std::string& path,
                                     const std::string& entry,
                                     const std::string& profile,
                                     const std::vector<ShaderDefine>& defines,
                                     Com<T>& outShader,
                                     std::string* outError) {
        auto setError = [outError](const std::string& message) {
            if (outError)
                *outError = message;
        };
        // 读取源码（支持文件系统/MPQ）
        std::vector<uint8_t> source;
        Logger::info(str::format("War3Material: Attempting to load shader: ", path));
        if (!ShaderManager::loadResource(path, source)) {
             Logger::err("War3Material: Failed to load shader source: " + path);
             setError("无法加载文件: " + path);
             return false;
        }
        Logger::info(str::format("War3Material: Loaded shader source (", source.size(), " bytes): ", path));
        Logger::info(str::format("War3Material: Entry point: ", entry, ", Profile: ", profile));

        // 载入编译器
        static HMODULE hCompiler = LoadLibraryA("d3dcompiler_47.dll");
        if (!hCompiler) {
            Logger::err("War3Material: d3dcompiler_47.dll not found!");
            setError("缺少 d3dcompiler_47.dll");
            return false;
        }
        
        auto fnCompile = (pD3DCompile)GetProcAddress(hCompiler, "D3DCompile");
        if (!fnCompile) {
            setError("获取 D3DCompile 失败");
            return false;
        }
        auto fnGetBlobPart = (pD3DGetBlobPart)GetProcAddress(hCompiler, "D3DGetBlobPart");
        static bool s_loggedShaderModel = false;
        if (!s_loggedShaderModel && device && device->GetOptions()) {
            Logger::info(str::format("War3Material: D3D9 shaderModel=", device->GetOptions()->shaderModel));
            s_loggedShaderModel = true;
        }

        ID3DBlob* code = nullptr;
        ID3DBlob* error = nullptr;

        std::vector<D3D_SHADER_MACRO> macros;
        if (!defines.empty()) {
            macros.reserve(defines.size() + 1);
            for (const auto& define : defines) {
                macros.push_back({ define.name.c_str(), define.value.c_str() });
            }
            macros.push_back({ nullptr, nullptr });
        }

        ShaderIncludeHandler includeHandler(GetDirectory(path));
        ID3DInclude* includePtr = &includeHandler;
        const D3D_SHADER_MACRO* macroPtr = macros.empty() ? nullptr : macros.data();

        const UINT compileFlags = D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY;
        HRESULT hr = fnCompile(source.data(), source.size(), path.c_str(), macroPtr, includePtr,
                               entry.c_str(), profile.c_str(), compileFlags, 0, &code, &error);

        if (FAILED(hr)) {
            if (error) {
                const char* errText = static_cast<const char*>(error->GetBufferPointer());
                Logger::err(str::format("Shader Compile Error (", path, "): ", errText));
                setError(errText ? std::string(errText) : std::string("编译失败"));
                error->Release();
            } else {
                setError("编译失败");
            }
            return false;
        }

        const void* shaderData = code ? code->GetBufferPointer() : nullptr;
        size_t shaderSize = code ? code->GetBufferSize() : 0u;
        std::vector<uint8_t> dxsoBuffer;
        std::vector<uint32_t> dxsoDwords;

        if (shaderData && shaderSize >= sizeof(uint32_t)) {
            uint32_t initialMagic = 0;
            ReadU32(reinterpret_cast<const uint8_t*>(shaderData), shaderSize, 0, initialMagic);
            Logger::info(str::format("War3Material: Initial shader magic=0x", std::hex, initialMagic, std::dec, " size=", shaderSize, " path=", path));
        }

        // DXBC -> DXSO 转换（D3D9 需要 legacy 字节码）
        if (shaderData && shaderSize >= sizeof(uint32_t)) {
            uint32_t magic = *reinterpret_cast<const uint32_t*>(shaderData);
            if (magic == kDxbcMagic && fnGetBlobPart) {
                ID3DBlob* legacy = nullptr;
                HRESULT hrLegacy = fnGetBlobPart(shaderData, shaderSize, D3D_BLOB_LEGACY_SHADER, 0, &legacy);
                Logger::info(str::format("War3Material: D3DGetBlobPart(LEGACY) hr=0x", std::hex, hrLegacy, std::dec, " path=", path));
                if (SUCCEEDED(hrLegacy) && legacy) {
                    code->Release();
                    code = legacy;
                    shaderData = code->GetBufferPointer();
                    shaderSize = code->GetBufferSize();
                    magic = *reinterpret_cast<const uint32_t*>(shaderData);
                }
            }

            if (magic == kDxbcMagic) {
                std::string extractError;
                if (!ExtractDxsoFromDxbc(shaderData, shaderSize, dxsoBuffer, &extractError)) {
                    setError("DXBC 转换失败: " + extractError);
                    if (error) error->Release();
                    if (code) code->Release();
                    return false;
                }
                shaderData = dxsoBuffer.data();
                shaderSize = dxsoBuffer.size();
                magic = *reinterpret_cast<const uint32_t*>(shaderData);
            }

            if (!IsDxsoToken(magic)) {
                setError("字节码格式不兼容 D3D9");
                if (error) error->Release();
                if (code) code->Release();
                return false;
            }
        }

        if (shaderData && shaderSize >= sizeof(uint32_t)) {
            uint32_t token0 = 0;
            ReadU32(reinterpret_cast<const uint8_t*>(shaderData), shaderSize, 0, token0);
            Logger::info(str::format("War3Material: DXSO token0=0x", std::hex, token0, std::dec, " size=", shaderSize, " path=", path));

            uint32_t expectedToken = 0;
            if (TryGetDxsoTokenFromProfile(profile, expectedToken) && token0 != expectedToken) {
                setError(str::format("DXSO 版本不匹配 got=0x", std::hex, token0, " expect=0x", expectedToken, std::dec));
                if (error) error->Release();
                if (code) code->Release();
                return false;
            }
            if ((shaderSize & 3u) != 0u) {
                setError("DXSO 大小不是 4 字节对齐");
                if (error) error->Release();
                if (code) code->Release();
                return false;
            }

            // 交给 DXSO 解码器处理 END token，避免误判导致截断
        }

        if (shaderData && shaderSize > 0) {
            if (shaderSize & 3u) {
                setError("DXSO 大小不是 4 字节对齐");
                if (error) error->Release();
                if (code) code->Release();
                return false;
            }
            const size_t dwordCount = shaderSize / sizeof(uint32_t);
            dxsoDwords.resize(dwordCount);
            std::memcpy(dxsoDwords.data(), shaderData, shaderSize);
            shaderData = dxsoDwords.data();
            shaderSize = dxsoDwords.size() * sizeof(uint32_t);
        }

        // DXSO 解析预检（提前捕获具体失败原因）
        if (shaderData) {
            try {
                DxsoReader reader(reinterpret_cast<const char*>(shaderData));
                DxsoModule module(reader);
                module.analyze();
            } catch (const DxvkError& e) {
                const std::string message = str::format("DXSO 解析失败: ", e.message());
                Logger::err("War3Material: " + message);
                setError(message);
                if (error) error->Release();
                if (code) code->Release();
                return false;
            }
        }

        // 创建 D3D9 着色器对象
        if constexpr (std::is_same_v<T, IDirect3DVertexShader9>) {
            hr = device->CreateVertexShader((DWORD*)shaderData, &outShader);
        } else {
            hr = device->CreatePixelShader((DWORD*)shaderData, &outShader);
        }

        code->Release();
        if (error) error->Release();

        if (FAILED(hr)) {
            const bool isVS = std::is_same_v<T, IDirect3DVertexShader9>;
            const std::string stageName = isVS ? "VS" : "PS";
            const std::string message = str::format(stageName, " 创建失败: 0x", std::hex, hr, " (", path, ")");
            Logger::err("War3Material: " + message);
            // 失败时尝试用 DXSO 编译路径诊断更具体的错误原因
            const std::string diag = DiagnoseDxsoFailure(device, shaderData, shaderSize, path, isVS);
            if (!diag.empty()) {
                Logger::err("War3Material: DXSO 诊断失败: " + diag);
                setError(message + " | DXSO 诊断失败: " + diag);
            } else {
                setError(message);
            }
            return false;
        }

        return true;
    }
}
