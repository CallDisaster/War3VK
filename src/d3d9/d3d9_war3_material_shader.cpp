// Material shaders: the execution-end half of war3::War3Material::compileShader
// (war3/shader/war3_material.cpp). The data layer compiles HLSL with
// d3dcompiler and extracts the DXSO tokens; parsing the module and creating
// the D3D9 shader object belong to the device (war3::IDeviceServices).

#include "d3d9_device.h"

#include "../dxso/dxso_module.h"
#include "../dxso/dxso_modinfo.h"

#include <type_traits>

namespace dxvk {

namespace {

// The DXSO compile path the device itself would take, for a failure reason
// more specific than the HRESULT.
std::string DiagnoseDxsoFailure(D3D9DeviceEx* device, const void* shaderData,
                                size_t shaderSize, const std::string& path,
                                bool isVertex) {
  if (!device || !shaderData || shaderSize < sizeof(uint32_t))
    return std::string();
  const uint32_t token0 = reinterpret_cast<const uint32_t*>(shaderData)[0];
  const uint32_t headerMask = token0 & 0xFFFF0000u;
  if (headerMask != 0xFFFF0000u && headerMask != 0xFFFE0000u) {
    return str::format("DXSO Header 无效 token0=0x", std::hex, token0, std::dec);
  }
  try {
    DxsoModuleInfo moduleInfo;
    moduleInfo.options = DxsoOptions(device->GetDXVKDevice(),
        *device->GetOptions(), device->GetVertexConstantLayout().floatSize());
    DxsoReader reader(reinterpret_cast<const char*>(shaderData));
    DxsoModule module(reader);
    DxsoAnalysisInfo info = module.analyze();
    const D3D9ConstantLayout& layout = isVertex
        ? device->GetVertexConstantLayout() : device->GetPixelConstantLayout();
    module.compile(moduleInfo, path, info, layout);
  } catch (const DxvkError& e) {
    return e.message();
  }
  return std::string();
}

template <typename T>
bool CreateMaterialShader(D3D9DeviceEx* device, const uint32_t* tokens,
                          size_t bytes, const std::string& path, T** shader,
                          std::string* error) {
  auto setError = [error](const std::string& message) {
    if (error)
      *error = message;
  };
  // DXSO 解析预检（提前捕获具体失败原因）
  if (tokens) {
    try {
      DxsoReader reader(reinterpret_cast<const char*>(tokens));
      DxsoModule module(reader);
      module.analyze();
    } catch (const DxvkError& e) {
      const std::string message = str::format("DXSO 解析失败: ", e.message());
      Logger::err("War3Material: " + message);
      setError(message);
      return false;
    }
  }

  // 创建 D3D9 着色器对象
  constexpr bool isVS = std::is_same_v<T, IDirect3DVertexShader9>;
  HRESULT hr;
  if constexpr (isVS)
    hr = device->CreateVertexShader(reinterpret_cast<const DWORD*>(tokens), shader);
  else
    hr = device->CreatePixelShader(reinterpret_cast<const DWORD*>(tokens), shader);
  if (FAILED(hr)) {
    const std::string stageName = isVS ? "VS" : "PS";
    const std::string message = str::format(stageName, " 创建失败: 0x", std::hex, hr, " (", path, ")");
    Logger::err("War3Material: " + message);
    // 失败时尝试用 DXSO 编译路径诊断更具体的错误原因
    const std::string diag = DiagnoseDxsoFailure(device, tokens, bytes, path, isVS);
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

} // namespace

bool D3D9DeviceEx::War3CreateMaterialVertexShader(const uint32_t* tokens, size_t bytes,
    const std::string& path, IDirect3DVertexShader9** shader, std::string* error) {
  return CreateMaterialShader(this, tokens, bytes, path, shader, error);
}

bool D3D9DeviceEx::War3CreateMaterialPixelShader(const uint32_t* tokens, size_t bytes,
    const std::string& path, IDirect3DPixelShader9** shader, std::string* error) {
  return CreateMaterialShader(this, tokens, bytes, path, shader, error);
}

} // namespace dxvk
