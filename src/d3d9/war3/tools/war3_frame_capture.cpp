#include "war3_frame_capture.h"

#include "../../d3d9_device.h"
#include "../../d3d9_surface.h"
#include "../../d3d9_war3_debug.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <vector>

namespace dxvk::war3::tools {

namespace {

using json = nlohmann::json;

std::string FormatHresult(HRESULT hr) {
  std::ostringstream ss;
  ss << "0x" << std::hex << std::uppercase
     << static_cast<uint32_t>(hr & 0xFFFFFFFFu);
  return ss.str();
}

uint64_t GetEpochMs() {
  using namespace std::chrono;
  return static_cast<uint64_t>(
      duration_cast<milliseconds>(system_clock::now().time_since_epoch())
          .count());
}

std::string BuildWarVkTempDir() {
  char exePath[MAX_PATH] = {0};
  if (GetModuleFileNameA(nullptr, exePath, MAX_PATH) <= 0)
    return {};

  std::filesystem::path exeDir(exePath);
  exeDir = exeDir.parent_path();
  if (exeDir.empty())
    return {};

  std::filesystem::path tempDir = exeDir / "WarVK" / "Temp";
  std::error_code ec;
  std::filesystem::create_directories(tempDir, ec);
  if (ec)
    return {};
  return tempDir.string();
}

const std::string& GetFrameCaptureRequestPath() {
  static const std::string s_path = []() {
    const std::string dir = BuildWarVkTempDir();
    if (dir.empty())
      return std::string();
    return (std::filesystem::path(dir) / "frame_capture_request.json").string();
  }();
  return s_path;
}

const std::string& GetFrameCaptureResultPath() {
  static const std::string s_path = []() {
    const std::string dir = BuildWarVkTempDir();
    if (dir.empty())
      return std::string();
    return (std::filesystem::path(dir) / "frame_capture_result.json").string();
  }();
  return s_path;
}

void WriteCaptureResultJson(const War3FrameCaptureResult& result) {
  const std::string& resultPath = GetFrameCaptureResultPath();
  if (resultPath.empty())
    return;

  json payload = {
      {"timestampMs", GetEpochMs()},
      {"source", "present-final-backbuffer"},
      {"requestId", result.requestId},
      {"ok", result.ok},
      {"outputPath", result.outputPath},
      {"width", result.width},
      {"height", result.height},
      {"format", "bmp"},
      {"error", result.error},
      {"pid", static_cast<uint32_t>(GetCurrentProcessId())},
  };

  std::ofstream file(resultPath, std::ios::binary | std::ios::trunc);
  if (!file.is_open())
    return;
  file << payload.dump(2);
}

bool WriteBmp24(const std::filesystem::path& outputPath, const uint8_t* pixels,
                uint32_t width, uint32_t height, uint32_t rowPitch,
                std::string* outError) {
  if (!pixels || width == 0 || height == 0 || rowPitch < width * 4u) {
    if (outError)
      *outError = "无效的像素缓冲";
    return false;
  }

  std::error_code ec;
  std::filesystem::create_directories(outputPath.parent_path(), ec);
  if (ec) {
    if (outError)
      *outError = "创建截图目录失败";
    return false;
  }

  const uint32_t dstRowPitch = (width * 3u + 3u) & ~3u;
  const uint32_t imageSize = dstRowPitch * height;

#pragma pack(push, 1)
  struct BmpFileHeader {
    uint16_t type = 0x4D42;
    uint32_t size = 0;
    uint16_t reserved1 = 0;
    uint16_t reserved2 = 0;
    uint32_t offBits = 0;
  };

  struct BmpInfoHeader {
    uint32_t size = 40;
    int32_t width = 0;
    int32_t height = 0;
    uint16_t planes = 1;
    uint16_t bitCount = 24;
    uint32_t compression = 0;
    uint32_t sizeImage = 0;
    int32_t xPelsPerMeter = 2835;
    int32_t yPelsPerMeter = 2835;
    uint32_t clrUsed = 0;
    uint32_t clrImportant = 0;
  };
#pragma pack(pop)

  BmpFileHeader fileHeader;
  fileHeader.offBits = sizeof(BmpFileHeader) + sizeof(BmpInfoHeader);
  fileHeader.size = fileHeader.offBits + imageSize;

  BmpInfoHeader infoHeader;
  infoHeader.width = static_cast<int32_t>(width);
  infoHeader.height = static_cast<int32_t>(height);
  infoHeader.sizeImage = imageSize;

  std::ofstream file(outputPath, std::ios::binary | std::ios::trunc);
  if (!file.is_open()) {
    if (outError)
      *outError = "打开截图文件失败";
    return false;
  }

  file.write(reinterpret_cast<const char*>(&fileHeader), sizeof(fileHeader));
  file.write(reinterpret_cast<const char*>(&infoHeader), sizeof(infoHeader));

  std::vector<uint8_t> row(dstRowPitch, 0u);
  for (int32_t y = static_cast<int32_t>(height) - 1; y >= 0; y--) {
    const uint8_t* src = pixels + rowPitch * static_cast<uint32_t>(y);
    for (uint32_t x = 0; x < width; x++) {
      row[x * 3u + 0u] = src[x * 4u + 0u];
      row[x * 3u + 1u] = src[x * 4u + 1u];
      row[x * 3u + 2u] = src[x * 4u + 2u];
    }
    file.write(reinterpret_cast<const char*>(row.data()), row.size());
  }

  if (!file.good()) {
    if (outError)
      *outError = "写出 BMP 文件失败";
    return false;
  }

  return true;
}

bool CaptureSurfaceToBmp(D3D9DeviceEx* device, D3D9Surface* sourceSurface,
                         const std::filesystem::path& outputPath,
                         uint32_t* outWidth, uint32_t* outHeight,
                         std::string* outError) {
  if (!device || !sourceSurface) {
    if (outError)
      *outError = "无效的 device/sourceSurface";
    return false;
  }

  const VkExtent2D extent = sourceSurface->GetSurfaceExtent();
  if (extent.width == 0u || extent.height == 0u) {
    if (outError)
      *outError = "backbuffer 尺寸无效";
    return false;
  }

  Com<IDirect3DSurface9> captureSurface;
  HRESULT hr = device->CreateRenderTarget(
      extent.width, extent.height, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0,
      FALSE, &captureSurface, nullptr);
  if (FAILED(hr)) {
    if (outError)
      *outError = "CreateRenderTarget 失败: " + FormatHresult(hr);
    return false;
  }

  hr = device->StretchRect(sourceSurface, nullptr, captureSurface.ptr(), nullptr,
                           D3DTEXF_NONE);
  if (FAILED(hr)) {
    if (outError)
      *outError = "StretchRect(backbuffer->capture) 失败: " +
                  FormatHresult(hr);
    return false;
  }

  Com<IDirect3DSurface9> sysmemSurface;
  hr = device->CreateOffscreenPlainSurface(extent.width, extent.height,
                                           D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
                                           &sysmemSurface, nullptr);
  if (FAILED(hr)) {
    if (outError)
      *outError = "CreateOffscreenPlainSurface 失败: " + FormatHresult(hr);
    return false;
  }

  hr = device->GetRenderTargetData(captureSurface.ptr(), sysmemSurface.ptr());
  if (FAILED(hr)) {
    if (outError)
      *outError = "GetRenderTargetData 失败: " + FormatHresult(hr);
    return false;
  }

  D3DLOCKED_RECT lockedRect = {};
  hr = sysmemSurface->LockRect(&lockedRect, nullptr, D3DLOCK_READONLY);
  if (FAILED(hr)) {
    if (outError)
      *outError = "LockRect(readback) 失败: " + FormatHresult(hr);
    return false;
  }

  const bool writeOk =
      WriteBmp24(outputPath, reinterpret_cast<const uint8_t*>(lockedRect.pBits),
                 extent.width, extent.height, static_cast<uint32_t>(lockedRect.Pitch),
                 outError);
  sysmemSurface->UnlockRect();

  if (!writeOk)
    return false;

  if (outWidth)
    *outWidth = extent.width;
  if (outHeight)
    *outHeight = extent.height;
  return true;
}

War3FrameCaptureResult LoadCaptureRequest(const std::string& requestPath,
                                          bool* outParseOk) {
  War3FrameCaptureResult result = {};
  if (outParseOk)
    *outParseOk = false;

  std::ifstream file(requestPath, std::ios::binary);
  if (!file.is_open()) {
    result.error = "打开 frame_capture_request.json 失败";
    return result;
  }

  try {
    json request = json::parse(file);
    result.requestId = request.value("requestId", std::string());
    result.outputPath = request.value("outputPath", std::string());
    if (outParseOk)
      *outParseOk = true;
  } catch (const std::exception& e) {
    result.error = std::string("解析 frame capture request 失败: ") + e.what();
  }

  return result;
}

} // namespace

bool ProcessPendingFrameCapture(D3D9DeviceEx* device, D3D9Surface* sourceSurface,
                                War3FrameCaptureResult* outResult) {
  War3FrameCaptureResult result = {};
  const std::string& requestPath = GetFrameCaptureRequestPath();
  if (requestPath.empty()) {
    if (outResult)
      *outResult = result;
    return false;
  }

  const DWORD attrs = GetFileAttributesA(requestPath.c_str());
  if (attrs == INVALID_FILE_ATTRIBUTES) {
    if (outResult)
      *outResult = result;
    return false;
  }

  result.handled = true;

  bool parseOk = false;
  result = LoadCaptureRequest(requestPath, &parseOk);
  result.handled = true;

  std::error_code ec;
  std::filesystem::remove(requestPath, ec);

  if (!parseOk) {
    WriteCaptureResultJson(result);
    war3dbg::Print("DXVK War3Capture: request parse failed err=%s\n",
                   result.error.c_str());
    if (outResult)
      *outResult = result;
    return true;
  }

  if (result.outputPath.empty()) {
    result.error = "outputPath 为空";
    WriteCaptureResultJson(result);
    war3dbg::Print("DXVK War3Capture: request=%s invalid outputPath\n",
                   result.requestId.c_str());
    if (outResult)
      *outResult = result;
    return true;
  }

  std::filesystem::path outputPath(result.outputPath);
  if (!outputPath.is_absolute())
    outputPath = std::filesystem::absolute(outputPath, ec);

  std::string ext = outputPath.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });

  if (ext != ".bmp") {
    result.error = "内部截图当前仅支持 .bmp 输出";
    WriteCaptureResultJson(result);
    war3dbg::Print("DXVK War3Capture: request=%s unsupported ext=%s\n",
                   result.requestId.c_str(), outputPath.extension().string().c_str());
    if (outResult)
      *outResult = result;
    return true;
  }

  result.outputPath = outputPath.string();
  result.ok = CaptureSurfaceToBmp(device, sourceSurface, outputPath,
                                  &result.width, &result.height, &result.error);
  WriteCaptureResultJson(result);

  if (result.ok) {
    war3dbg::Print(
        "DXVK War3Capture: request=%s ok size=%ux%u out=%s\n",
        result.requestId.c_str(), result.width, result.height,
        result.outputPath.c_str());
  } else {
    war3dbg::Print("DXVK War3Capture: request=%s failed err=%s\n",
                   result.requestId.c_str(), result.error.c_str());
  }

  if (outResult)
    *outResult = result;
  return true;
}

} // namespace dxvk::war3::tools
