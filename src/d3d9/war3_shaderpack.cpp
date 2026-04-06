
/**
 * @file war3_shaderpack.cpp
 * @brief War3MapReforge Vulkan ShaderPack 运行时实现
 */

#define WAR3_SHADER_API_INTERNAL

#include "war3_shaderpack.h"
#include "war3_shaderpack_internal.h"
#include "d3d9_war3_pipeline.h"
#include "war3/core/war3_storm.h"
#include "war3/render/war3_render_state.h"

#include "../dxvk/dxvk_device.h"
#include "../dxvk/dxvk_buffer.h"
#include "../dxvk/dxvk_cmdlist.h"
#include "../dxvk/dxvk_image.h"
#include "../dxvk/dxvk_sampler.h"
#include "../dxvk/dxvk_util.h"
#include "../dxvk/dxvk_access.h"
#include "../util/util_string.h"
#include "../util/util_matrix.h"
#include "../util/log/log.h"

#include <war3_fullscreen_vert.h>

#define STBI_NO_STDIO
#include "../../MemHack/3rd/stb/stb_image.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace war3shader {

namespace {

using json = nlohmann::json;
using dxvk::Logger;

struct PackUBO {
    dxvk::Matrix4 view;
    dxvk::Matrix4 proj;
    dxvk::Matrix4 invView;
    dxvk::Matrix4 invProj;
    dxvk::Vector4 cameraTime;      // xyz = 相机位置, w = 游戏时间
    dxvk::Vector4 resolutionTime;  // xy = 分辨率, z = 帧时间, w = 总时间
};

struct PackPushConstants {
    uint32_t samplerIndices[8];
    dxvk::Vector4 params[SHADERPACK_MAX_PARAMS];
};

struct PackPass {
    std::string name;
    std::string file;
    std::vector<uint32_t> spirv;
    bool enabled = true;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkFormat pipelineFormat = VK_FORMAT_UNDEFINED;
};

struct PendingTextureUpload {
    uint32_t slot = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> pixels;
    std::string source;
};

struct PackState {
    std::string name;
    std::string path;
    std::vector<PackPass> passes;
    std::vector<uint32_t> shadowReceiverSpirv;
    ShaderPackError lastError = ShaderPackError::OK;
    bool loaded = false;
    bool enabled = false;
};

struct ShaderPackRuntime {
    dxvk::Rc<dxvk::DxvkDevice> device;
    const dxvk::DxvkPipelineLayout* layout = nullptr;
    dxvk::Rc<dxvk::DxvkSampler> samplerLinear;
    dxvk::Rc<dxvk::DxvkBuffer> uboBuffer;

    dxvk::Rc<dxvk::DxvkImage> colorCopy;
    dxvk::Rc<dxvk::DxvkImageView> colorCopyView;
    dxvk::Rc<dxvk::DxvkImage> pingImage;
    dxvk::Rc<dxvk::DxvkImageView> pingView;
    dxvk::Rc<dxvk::DxvkImage> pongImage;
    dxvk::Rc<dxvk::DxvkImageView> pongView;

    dxvk::Rc<dxvk::DxvkImage> fallbackImage;
    dxvk::Rc<dxvk::DxvkImageView> fallbackView;

    VkExtent3D cachedInputExtent = { 0, 0, 1 };
    VkExtent3D cachedPassExtent = { 0, 0, 1 };
    VkFormat cachedFormat = VK_FORMAT_UNDEFINED;

    float renderScale = 1.0f;

    PackState pack;
    std::array<dxvk::Vector4, SHADERPACK_MAX_PARAMS> params = { };
    std::array<dxvk::Rc<dxvk::DxvkImageView>, SHADERPACK_MAX_TEXTURES> textures = { };
    std::vector<PendingTextureUpload> pendingUploads;
    bool shadowReceiverEnabled = true;

    std::string lastErrorMessage;
    ShaderLogCallback logCallback = nullptr;
    void* logUserData = nullptr;
    uint32_t compileCount = 0;
    uint32_t errorCount = 0;

    std::chrono::steady_clock::time_point lastFrameTime;
    float totalTime = 0.0f;
    bool timeInitialized = false;

    std::mutex mutex;
};

ShaderPackRuntime g_runtime;

void LogMessage(const std::string& message) {
    Logger::info(message);
    if (g_runtime.logCallback) {
        g_runtime.logCallback(message.c_str(), g_runtime.logUserData);
    }
}

void SetError(ShaderPackError error, const std::string& message) {
    g_runtime.pack.lastError = error;
    g_runtime.lastErrorMessage = message;
    g_runtime.errorCount++;
    Logger::err("ShaderPack: " + message);
    if (g_runtime.logCallback) {
        g_runtime.logCallback(message.c_str(), g_runtime.logUserData);
    }
}

uint32_t BuildPackFlags() {
    uint32_t flags = PACK_FLAG_NONE;
    if (g_runtime.pack.loaded) {
        flags |= PACK_FLAG_LOADED;
    }
    if (g_runtime.pack.enabled) {
        flags |= PACK_FLAG_ENABLED;
    }
    if (g_runtime.pack.lastError != ShaderPackError::OK) {
        flags |= PACK_FLAG_HAS_ERROR;
    }
    return flags;
}

bool IsAbsolutePath(const std::string& path) {
    if (path.size() >= 2 && path[1] == ':')
        return true;
    if (!path.empty() && (path[0] == '\\' || path[0] == '/'))
        return true;
    return false;
}

std::string JoinPath(const std::string& base, const std::string& file) {
    if (file.empty())
        return file;
    if (base.empty() || IsAbsolutePath(file))
        return file;
    char last = base.back();
    if (last == '/' || last == '\\')
        return base + file;
    return base + "/" + file;
}

bool LoadFileBinary(const std::string& path, std::vector<uint8_t>& outBuffer) {
    std::ifstream file(path, std::ios::binary);
    if (file.is_open()) {
        file.seekg(0, std::ios::end);
        size_t size = static_cast<size_t>(file.tellg());
        file.seekg(0, std::ios::beg);
        outBuffer.resize(size);
        file.read(reinterpret_cast<char*>(outBuffer.data()), size);
        return true;
    }

    std::vector<uint8_t> mpqBuffer;
    if (dxvk::war3::War3Storm::get().loadFile(path, mpqBuffer)) {
        outBuffer = std::move(mpqBuffer);
        return true;
    }

    return false;
}

bool LoadFileText(const std::string& path, std::string& outText) {
    std::ifstream file(path);
    if (file.is_open()) {
        std::stringstream ss;
        ss << file.rdbuf();
        outText = ss.str();
        return true;
    }

    std::vector<uint8_t> buffer;
    if (dxvk::war3::War3Storm::get().loadFile(path, buffer)) {
        outText.assign(buffer.begin(), buffer.end());
        return true;
    }

    return false;
}

std::string FormatJsonError(const std::string& text, const json::parse_error& error) {
    size_t pos = error.byte;
    size_t line = 1;
    size_t column = 1;
    for (size_t i = 0; i < text.size() && i < pos; i++) {
        if (text[i] == '\n') {
            line++;
            column = 1;
        } else {
            column++;
        }
    }
    return dxvk::str::format("JSON 解析失败: line ", line, " col ", column, " (", error.what(), ")");
}

bool DecodeImageRgba(const std::string& path, PendingTextureUpload& outUpload, ShaderPackError& outCode, std::string& outError) {
    std::vector<uint8_t> bytes;
    if (!LoadFileBinary(path, bytes)) {
        outError = dxvk::str::format("找不到纹理: ", path);
        outCode = ShaderPackError::TEXTURE_NOT_FOUND;
        return false;
    }
    if (bytes.empty()) {
        outError = dxvk::str::format("纹理文件为空: ", path);
        outCode = ShaderPackError::TEXTURE_LOAD_ERROR;
        return false;
    }
    if (bytes.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        outError = dxvk::str::format("纹理过大: ", path);
        outCode = ShaderPackError::TEXTURE_LOAD_ERROR;
        return false;
    }

    int width = 0;
    int height = 0;
    int comp = 0;
    stbi_uc* pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()),
                                            &width, &height, &comp, 4);
    if (!pixels) {
        outError = dxvk::str::format("纹理解码失败: ", path);
        outCode = ShaderPackError::TEXTURE_LOAD_ERROR;
        return false;
    }
    if (width <= 0 || height <= 0) {
        stbi_image_free(pixels);
        outError = dxvk::str::format("纹理尺寸非法: ", path);
        outCode = ShaderPackError::TEXTURE_LOAD_ERROR;
        return false;
    }

    const size_t size = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
    outUpload.width = static_cast<uint32_t>(width);
    outUpload.height = static_cast<uint32_t>(height);
    outUpload.pixels.assign(pixels, pixels + size);
    outUpload.source = path;
    stbi_image_free(pixels);
    outCode = ShaderPackError::OK;
    outError.clear();
    return true;
}

bool LoadSpirvFile(const std::string& path, std::vector<uint32_t>& outSpirv) {
    std::vector<uint8_t> bytes;
    if (!LoadFileBinary(path, bytes)) {
        return false;
    }
    if (bytes.empty() || (bytes.size() % 4) != 0) {
        return false;
    }
    outSpirv.resize(bytes.size() / 4);
    std::memcpy(outSpirv.data(), bytes.data(), bytes.size());
    return true;
}

bool HasValidSpirv(const std::vector<uint32_t>& spirv) {
    if (spirv.size() < 4)
        return false;
    return spirv[0] == 0x07230203u;
}

void DestroyPipeline(VkPipeline pipeline) {
    if (pipeline == VK_NULL_HANDLE || !g_runtime.device)
        return;
    g_runtime.device->vkd()->vkDestroyPipeline(g_runtime.device->vkd()->device(), pipeline, nullptr);
}

void ClearPack() {
    for (auto& pass : g_runtime.pack.passes) {
        DestroyPipeline(pass.pipeline);
        pass.pipeline = VK_NULL_HANDLE;
        pass.pipelineFormat = VK_FORMAT_UNDEFINED;
    }
    g_runtime.pack.passes.clear();
    g_runtime.pack.shadowReceiverSpirv.clear();
    g_runtime.pack.loaded = false;
    g_runtime.pack.enabled = false;
    g_runtime.pack.lastError = ShaderPackError::OK;
    g_runtime.pack.name.clear();
    g_runtime.pack.path.clear();
    g_runtime.pendingUploads.clear();
    g_runtime.shadowReceiverEnabled = true;
}

void InvalidatePassPipelines() {
    for (auto& pass : g_runtime.pack.passes) {
        DestroyPipeline(pass.pipeline);
        pass.pipeline = VK_NULL_HANDLE;
        pass.pipelineFormat = VK_FORMAT_UNDEFINED;
    }
}

void EnsureSampler() {
    if (g_runtime.samplerLinear)
        return;
    dxvk::DxvkSamplerKey key = { };
    key.setFilter(VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST);
    key.setAddressModes(
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    key.setUsePixelCoordinates(false);
    g_runtime.samplerLinear = g_runtime.device->createSampler(key);
}

void EnsureLayout() {
    if (g_runtime.layout)
        return;
    std::array<dxvk::DxvkDescriptorSetLayoutBinding, 8> bindings = {
        dxvk::DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT),
        dxvk::DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT),
        dxvk::DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT),
        dxvk::DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT),
        dxvk::DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT),
        dxvk::DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT),
        dxvk::DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT),
        dxvk::DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT),
    };

    g_runtime.layout = g_runtime.device->createBuiltInPipelineLayout(
        dxvk::DxvkPipelineLayoutFlag::UsesSamplerHeap,
        VK_SHADER_STAGE_FRAGMENT_BIT,
        sizeof(PackPushConstants),
        bindings.size(),
        bindings.data());
}

void EnsureUboBuffer() {
    if (g_runtime.uboBuffer)
        return;
    dxvk::DxvkBufferCreateInfo info = { };
    info.size = sizeof(PackUBO);
    info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    info.access = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_UNIFORM_READ_BIT;
    info.debugName = "War3ShaderPackUBO";
    g_runtime.uboBuffer = g_runtime.device->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
}

void EnsureFallbackTexture() {
    if (g_runtime.fallbackImage)
        return;
    dxvk::DxvkImageCreateInfo info = { };
    info.type = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.sampleCount = VK_SAMPLE_COUNT_1_BIT;
    info.extent = { 1, 1, 1 };
    info.numLayers = 1;
    info.mipLevels = 1;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    info.access = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    info.debugName = "War3ShaderPackFallback";

    g_runtime.fallbackImage = g_runtime.device->createImage(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    VkComponentMapping mapping = {
        VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY
    };

    dxvk::DxvkImageViewKey viewKey = { };
    viewKey.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewKey.format = info.format;
    viewKey.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    viewKey.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    viewKey.aspects = VK_IMAGE_ASPECT_COLOR_BIT;
    viewKey.mipIndex = 0;
    viewKey.mipCount = 1;
    viewKey.layerIndex = 0;
    viewKey.layerCount = 1;
    viewKey.packedSwizzle = dxvk::DxvkImageViewKey::packSwizzle(mapping);
    g_runtime.fallbackView = g_runtime.fallbackImage->createView(viewKey);
}

bool UploadTexture(const dxvk::Rc<dxvk::DxvkCommandList>& ctx, const PendingTextureUpload& upload) {
    if (!ctx || !g_runtime.device)
        return false;
    if (upload.width == 0 || upload.height == 0 || upload.pixels.empty())
        return false;

    dxvk::DxvkImageCreateInfo info = { };
    info.type = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.sampleCount = VK_SAMPLE_COUNT_1_BIT;
    info.extent = { upload.width, upload.height, 1 };
    info.numLayers = 1;
    info.mipLevels = 1;
    info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    info.access = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    info.debugName = "War3ShaderPackTexture";

    auto image = g_runtime.device->createImage(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!image)
        return false;

    VkComponentMapping mapping = {
        VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY
    };

    dxvk::DxvkImageViewKey viewKey = { };
    viewKey.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewKey.format = info.format;
    viewKey.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    viewKey.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    viewKey.aspects = VK_IMAGE_ASPECT_COLOR_BIT;
    viewKey.mipIndex = 0;
    viewKey.mipCount = 1;
    viewKey.layerIndex = 0;
    viewKey.layerCount = 1;
    viewKey.packedSwizzle = dxvk::DxvkImageViewKey::packSwizzle(mapping);
    auto view = image->createView(viewKey);

    const size_t dataSize = upload.pixels.size();
    dxvk::DxvkBufferCreateInfo bufferInfo = { };
    bufferInfo.size = dataSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferInfo.stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
    bufferInfo.access = VK_ACCESS_TRANSFER_READ_BIT;
    bufferInfo.debugName = "War3ShaderPackTextureUpload";

    auto uploadBuffer = g_runtime.device->createBuffer(
        bufferInfo,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!uploadBuffer)
        return false;

    std::memcpy(uploadBuffer->mapPtr(0), upload.pixels.data(), dataSize);
    auto uploadSlice = uploadBuffer->getSliceInfo();

    VkImageMemoryBarrier2 barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = image->pickLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image->handle();
    barrier.subresourceRange = image->getAvailableSubresources();

    VkDependencyInfo depInfo = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    depInfo.imageMemoryBarrierCount = 1;
    depInfo.pImageMemoryBarriers = &barrier;
    ctx->cmdPipelineBarrier(dxvk::DxvkCmdBuffer::InitBuffer, &depInfo);

    VkBufferImageCopy2 imageRegion = { VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2 };
    imageRegion.bufferOffset = uploadSlice.offset;
    imageRegion.imageExtent = info.extent;
    imageRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imageRegion.imageSubresource.layerCount = 1u;

    VkCopyBufferToImageInfo2 imageCopy = { VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2 };
    imageCopy.srcBuffer = uploadSlice.buffer;
    imageCopy.dstImage = image->handle();
    imageCopy.dstImageLayout = image->pickLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    imageCopy.regionCount = 1;
    imageCopy.pRegions = &imageRegion;
    ctx->cmdCopyBufferToImage(dxvk::DxvkCmdBuffer::InitBuffer, &imageCopy);

    barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    barrier.dstAccessMask = info.access;
    barrier.dstStageMask = info.stages;
    barrier.oldLayout = image->pickLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    barrier.newLayout = info.layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image->handle();
    barrier.subresourceRange = image->getAvailableSubresources();

    depInfo = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    depInfo.imageMemoryBarrierCount = 1;
    depInfo.pImageMemoryBarriers = &barrier;
    ctx->cmdPipelineBarrier(dxvk::DxvkCmdBuffer::InitBuffer, &depInfo);

    image->trackLayout(image->getAvailableSubresources(), info.layout);

    ctx->track(uploadBuffer, dxvk::DxvkAccess::Read);
    ctx->track(image, dxvk::DxvkAccess::Write);

    if (upload.slot < g_runtime.textures.size()) {
        g_runtime.textures[upload.slot] = std::move(view);
    }
    return true;
}

void ProcessPendingUploads(const dxvk::Rc<dxvk::DxvkCommandList>& ctx) {
    if (g_runtime.pendingUploads.empty())
        return;
    EnsureFallbackTexture();
    for (const auto& upload : g_runtime.pendingUploads) {
        if (!UploadTexture(ctx, upload)) {
            SetError(ShaderPackError::TEXTURE_LOAD_ERROR,
                     dxvk::str::format("纹理上传失败: ", upload.source));
        }
    }
    g_runtime.pendingUploads.clear();
}

void CreateColorCopy(VkExtent3D extent, VkFormat format) {
    dxvk::DxvkImageCreateInfo info = { };
    info.type = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.sampleCount = VK_SAMPLE_COUNT_1_BIT;
    info.extent = extent;
    info.numLayers = 1;
    info.mipLevels = 1;
    info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    info.access = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    info.debugName = "War3ShaderPackColorCopy";

    g_runtime.colorCopy = g_runtime.device->createImage(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    VkComponentMapping mapping = {
        VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY
    };

    dxvk::DxvkImageViewKey viewKey = { };
    viewKey.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewKey.format = format;
    viewKey.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    viewKey.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    viewKey.aspects = VK_IMAGE_ASPECT_COLOR_BIT;
    viewKey.mipIndex = 0;
    viewKey.mipCount = 1;
    viewKey.layerIndex = 0;
    viewKey.layerCount = 1;
    viewKey.packedSwizzle = dxvk::DxvkImageViewKey::packSwizzle(mapping);
    g_runtime.colorCopyView = g_runtime.colorCopy->createView(viewKey);
}

void CreatePingPong(VkExtent3D extent, VkFormat format) {
    dxvk::DxvkImageCreateInfo info = { };
    info.type = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.sampleCount = VK_SAMPLE_COUNT_1_BIT;
    info.extent = extent;
    info.numLayers = 1;
    info.mipLevels = 1;
    info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    info.stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    info.access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    info.debugName = "War3ShaderPackPingPong";

    g_runtime.pingImage = g_runtime.device->createImage(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    g_runtime.pongImage = g_runtime.device->createImage(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    VkComponentMapping mapping = {
        VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY
    };

    dxvk::DxvkImageViewKey viewKey = { };
    viewKey.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewKey.format = format;
    viewKey.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    viewKey.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    viewKey.aspects = VK_IMAGE_ASPECT_COLOR_BIT;
    viewKey.mipIndex = 0;
    viewKey.mipCount = 1;
    viewKey.layerIndex = 0;
    viewKey.layerCount = 1;
    viewKey.packedSwizzle = dxvk::DxvkImageViewKey::packSwizzle(mapping);

    g_runtime.pingView = g_runtime.pingImage->createView(viewKey);
    g_runtime.pongView = g_runtime.pongImage->createView(viewKey);
}
VkExtent3D CalcPassExtent(const VkExtent3D& inputExtent) {
    float scale = std::clamp(g_runtime.renderScale, 0.25f, 2.0f);
    VkExtent3D extent = inputExtent;
    extent.width = std::max<uint32_t>(1u, static_cast<uint32_t>(std::round(float(inputExtent.width) * scale)));
    extent.height = std::max<uint32_t>(1u, static_cast<uint32_t>(std::round(float(inputExtent.height) * scale)));
    return extent;
}

void EnsureResources(const dxvk::War3PipelineInput& input) {
    VkExtent3D inputExtent = input.colorView->mipLevelExtent(0u);
    VkFormat format = input.colorView->image()->info().format;
    VkExtent3D passExtent = CalcPassExtent(inputExtent);

    const bool extentChanged = inputExtent.width != g_runtime.cachedInputExtent.width
        || inputExtent.height != g_runtime.cachedInputExtent.height
        || passExtent.width != g_runtime.cachedPassExtent.width
        || passExtent.height != g_runtime.cachedPassExtent.height;

    const bool formatChanged = format != g_runtime.cachedFormat;

    if (!extentChanged && !formatChanged && g_runtime.colorCopyView && g_runtime.pingView && g_runtime.pongView) {
        return;
    }

    g_runtime.cachedInputExtent = inputExtent;
    g_runtime.cachedPassExtent = passExtent;
    g_runtime.cachedFormat = format;

    g_runtime.colorCopy = nullptr;
    g_runtime.colorCopyView = nullptr;
    g_runtime.pingImage = nullptr;
    g_runtime.pingView = nullptr;
    g_runtime.pongImage = nullptr;
    g_runtime.pongView = nullptr;

    CreateColorCopy(inputExtent, format);
    CreatePingPong(passExtent, format);

    InvalidatePassPipelines();
}

void CopyColorToInput(const dxvk::Rc<dxvk::DxvkCommandList>& ctx, const dxvk::Rc<dxvk::DxvkImageView>& srcView) {
    if (!g_runtime.colorCopy || !g_runtime.colorCopyView || !srcView)
        return;

    VkImageLayout srcLayout = srcView->getLayout();

    VkImageMemoryBarrier2 barriers[2] = { };
    for (auto& barrier : barriers) {
        barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    }

    barriers[0].srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    barriers[0].srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    barriers[0].dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barriers[0].dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    barriers[0].oldLayout = srcLayout;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barriers[0].image = srcView->image()->handle();
    barriers[0].subresourceRange = srcView->imageSubresources();

    barriers[1].srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    barriers[1].srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
    barriers[1].dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barriers[1].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barriers[1].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[1].image = g_runtime.colorCopy->handle();
    barriers[1].subresourceRange = g_runtime.colorCopyView->imageSubresources();

    VkDependencyInfo depInfo = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    depInfo.imageMemoryBarrierCount = 2;
    depInfo.pImageMemoryBarriers = barriers;
    ctx->cmdPipelineBarrier(dxvk::DxvkCmdBuffer::ExecBuffer, &depInfo);

    VkImageCopy2 copyRegion = { VK_STRUCTURE_TYPE_IMAGE_COPY_2 };
    copyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copyRegion.srcSubresource.layerCount = 1;
    copyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copyRegion.dstSubresource.layerCount = 1;
    copyRegion.extent = srcView->image()->info().extent;

    VkCopyImageInfo2 copyInfo = { VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2 };
    copyInfo.srcImage = srcView->image()->handle();
    copyInfo.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    copyInfo.dstImage = g_runtime.colorCopy->handle();
    copyInfo.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    copyInfo.regionCount = 1;
    copyInfo.pRegions = &copyRegion;

    ctx->cmdCopyImage(dxvk::DxvkCmdBuffer::ExecBuffer, &copyInfo);

    barriers[0].srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barriers[0].srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    barriers[0].dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    barriers[0].dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barriers[0].newLayout = srcLayout;

    barriers[1].srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barriers[1].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barriers[1].dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    barriers[1].dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
    barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    ctx->cmdPipelineBarrier(dxvk::DxvkCmdBuffer::ExecBuffer, &depInfo);

    ctx->track(srcView->image(), dxvk::DxvkAccess::Read);
    ctx->track(g_runtime.colorCopy, dxvk::DxvkAccess::Write);
}

void UpdateUboOnCmd(const dxvk::Rc<dxvk::DxvkCommandList>& ctx,
                    const dxvk::War3PipelineInput& input,
                    VkExtent3D extent,
                    float frameTime,
                    float totalTime) {
    PackUBO ubo = { };
    if (input.scene.worldCamera.valid) {
        const dxvk::Matrix4 view = input.scene.worldCamera.view;
        const dxvk::Matrix4 proj = input.scene.worldCamera.proj;
        const dxvk::Matrix4 invView = dxvk::inverse(view);
        const dxvk::Matrix4 invProj = dxvk::inverse(proj);

        ubo.view = view;
        ubo.proj = proj;
        ubo.invView = invView;
        ubo.invProj = invProj;

        ubo.cameraTime = dxvk::Vector4(invView[3].x, invView[3].y, invView[3].z, dxvk::War3RenderState::GetGameTime());
    } else {
        ubo.view = dxvk::Matrix4();
        ubo.proj = dxvk::Matrix4();
        ubo.invView = dxvk::Matrix4();
        ubo.invProj = dxvk::Matrix4();
        ubo.cameraTime = dxvk::Vector4(0.0f, 0.0f, 0.0f, dxvk::War3RenderState::GetGameTime());
    }

    ubo.resolutionTime = dxvk::Vector4(
        float(extent.width),
        float(extent.height),
        frameTime,
        totalTime);

    auto uboInfo = g_runtime.uboBuffer->getSliceInfo(0u, sizeof(PackUBO));
    ctx->cmdUpdateBuffer(dxvk::DxvkCmdBuffer::ExecBuffer,
                         uboInfo.buffer,
                         uboInfo.offset,
                         sizeof(PackUBO),
                         &ubo);

    VkBufferMemoryBarrier2 barrier = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 };
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_UNIFORM_READ_BIT;
    barrier.buffer = uboInfo.buffer;
    barrier.offset = uboInfo.offset;
    barrier.size = sizeof(PackUBO);

    VkDependencyInfo depInfo = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    depInfo.bufferMemoryBarrierCount = 1;
    depInfo.pBufferMemoryBarriers = &barrier;
    ctx->cmdPipelineBarrier(dxvk::DxvkCmdBuffer::ExecBuffer, &depInfo);

    ctx->track(g_runtime.uboBuffer, dxvk::DxvkAccess::Write);
}

bool EnsurePassPipeline(PackPass& pass, VkFormat format) {
    if (pass.pipeline != VK_NULL_HANDLE && pass.pipelineFormat == format)
        return true;

    DestroyPipeline(pass.pipeline);
    pass.pipeline = VK_NULL_HANDLE;
    pass.pipelineFormat = VK_FORMAT_UNDEFINED;

    if (!HasValidSpirv(pass.spirv)) {
        return false;
    }

    dxvk::util::DxvkBuiltInGraphicsState state = { };
    state.vs = dxvk::util::DxvkBuiltInShaderStage(war3_fullscreen_vert, nullptr);
    state.fs.code = pass.spirv.data();
    state.fs.size = pass.spirv.size() * sizeof(uint32_t);
    state.fs.spec = nullptr;
    state.colorFormat = format;
    state.sampleCount = VK_SAMPLE_COUNT_1_BIT;

    pass.pipeline = g_runtime.device->createBuiltInGraphicsPipeline(g_runtime.layout, state);
    pass.pipelineFormat = format;
    return pass.pipeline != VK_NULL_HANDLE;
}

void TransitionDepthToReadOnly(const dxvk::Rc<dxvk::DxvkCommandList>& ctx,
                               const dxvk::Rc<dxvk::DxvkImageView>& depthView,
                               VkImageLayout& oldLayout) {
    if (!depthView)
        return;
    oldLayout = depthView->getLayout();
    if (oldLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL)
        return;

    VkImageMemoryBarrier2 barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    barrier.image = depthView->image()->handle();
    barrier.subresourceRange = depthView->imageSubresources();

    VkDependencyInfo depInfo = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    depInfo.imageMemoryBarrierCount = 1;
    depInfo.pImageMemoryBarriers = &barrier;
    ctx->cmdPipelineBarrier(dxvk::DxvkCmdBuffer::ExecBuffer, &depInfo);
}

void RestoreDepthLayout(const dxvk::Rc<dxvk::DxvkCommandList>& ctx,
                        const dxvk::Rc<dxvk::DxvkImageView>& depthView,
                        VkImageLayout oldLayout) {
    if (!depthView)
        return;
    if (oldLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL)
        return;

    VkImageMemoryBarrier2 barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    barrier.newLayout = oldLayout;
    barrier.image = depthView->image()->handle();
    barrier.subresourceRange = depthView->imageSubresources();

    VkDependencyInfo depInfo = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    depInfo.imageMemoryBarrierCount = 1;
    depInfo.pImageMemoryBarriers = &barrier;
    ctx->cmdPipelineBarrier(dxvk::DxvkCmdBuffer::ExecBuffer, &depInfo);
}

void TransitionImageForRender(const dxvk::Rc<dxvk::DxvkCommandList>& ctx,
                              const dxvk::Rc<dxvk::DxvkImageView>& view,
                              VkImageLayout newLayout) {
    VkImageMemoryBarrier2 barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.oldLayout = view->getLayout();
    barrier.newLayout = newLayout;
    barrier.image = view->image()->handle();
    barrier.subresourceRange = view->imageSubresources();

    VkDependencyInfo depInfo = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    depInfo.imageMemoryBarrierCount = 1;
    depInfo.pImageMemoryBarriers = &barrier;
    ctx->cmdPipelineBarrier(dxvk::DxvkCmdBuffer::ExecBuffer, &depInfo);
}

void TransitionImageToReadOnly(const dxvk::Rc<dxvk::DxvkCommandList>& ctx,
                               const dxvk::Rc<dxvk::DxvkImageView>& view,
                               VkImageLayout newLayout) {
    const bool toReadOnly = (newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    VkImageMemoryBarrier2 barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstStageMask = toReadOnly ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    barrier.dstAccessMask = toReadOnly ? VK_ACCESS_2_SHADER_READ_BIT : VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout = newLayout;
    barrier.image = view->image()->handle();
    barrier.subresourceRange = view->imageSubresources();

    VkDependencyInfo depInfo = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    depInfo.imageMemoryBarrierCount = 1;
    depInfo.pImageMemoryBarriers = &barrier;
    ctx->cmdPipelineBarrier(dxvk::DxvkCmdBuffer::ExecBuffer, &depInfo);
}
bool LoadDefaultPass(const std::string& packPath, const std::string& name, const std::string& file, std::vector<PackPass>& outPasses) {
    if (outPasses.size() >= SHADERPACK_MAX_PASSES) {
        SetError(ShaderPackError::TOO_MANY_PASSES, "Pass 数量超过上限");
        return false;
    }
    PackPass pass;
    pass.name = name;
    pass.file = file;

    std::string fullPath = JoinPath(packPath, file);
    if (!LoadSpirvFile(fullPath, pass.spirv)) {
        return false;
    }

    outPasses.push_back(std::move(pass));
    g_runtime.compileCount++;
    return true;
}

bool LoadPassFromJson(const std::string& packPath, const json& item, std::vector<PackPass>& outPasses) {
    if (!item.is_object())
        return false;
    if (outPasses.size() >= SHADERPACK_MAX_PASSES) {
        SetError(ShaderPackError::TOO_MANY_PASSES, "Pass 数量超过上限");
        return false;
    }

    std::string name = item.value("name", "pass");
    std::string file = item.value("shader", "");
    if (file.empty())
        return false;

    PackPass pass;
    pass.name = name;
    pass.file = file;
    pass.enabled = item.value("enabled", true);

    std::string fullPath = JoinPath(packPath, file);
    if (!LoadSpirvFile(fullPath, pass.spirv)) {
        SetError(ShaderPackError::SHADER_NOT_FOUND, dxvk::str::format("找不到 shader: ", fullPath));
        return false;
    }

    if (!HasValidSpirv(pass.spirv)) {
        SetError(ShaderPackError::SHADER_COMPILE_ERROR, dxvk::str::format("SPIR-V 无效: ", fullPath));
        return false;
    }

    outPasses.push_back(std::move(pass));
    g_runtime.compileCount++;
    return true;
}

void LoadShadowReceiver(const std::string& packPath, const json& root) {
    if (!root.contains("shadow_receiver"))
        return;
    if (!root["shadow_receiver"].is_string())
        return;

    std::string file = root["shadow_receiver"].get<std::string>();
    if (file.empty())
        return;

    std::string fullPath = JoinPath(packPath, file);
    std::vector<uint32_t> spirv;
    if (!LoadSpirvFile(fullPath, spirv)) {
        SetError(ShaderPackError::SHADER_NOT_FOUND, dxvk::str::format("找不到 ShadowReceiver: ", fullPath));
        return;
    }
    if (!HasValidSpirv(spirv)) {
        SetError(ShaderPackError::SHADER_COMPILE_ERROR, dxvk::str::format("ShadowReceiver SPIR-V 无效: ", fullPath));
        return;
    }

    g_runtime.pack.shadowReceiverSpirv = std::move(spirv);
}

void LoadParamsFromJson(const json& root) {
    if (!root.contains("params"))
        return;
    const auto& params = root["params"];
    
    // Clear existing params first? Or just overwrite. Default is 0.
    // Ensure we handle both array and object formats
    if (params.is_array()) {
        for (size_t i = 0; i < params.size() && i < SHADERPACK_MAX_PARAMS; ++i) {
             const auto& val = params[i];
             if (val.is_array() && val.size() >= 4) {
                 g_runtime.params[i] = dxvk::Vector4(
                     val[0].get<float>(), val[1].get<float>(),
                     val[2].get<float>(), val[3].get<float>()
                 );
             }
        }
    } else if (params.is_object()) {
        for (auto& [key, val] : params.items()) {
             try {
                 uint32_t idx = std::stoul(key);
                 if (idx < SHADERPACK_MAX_PARAMS && val.is_array() && val.size() >= 4) {
                     g_runtime.params[idx] = dxvk::Vector4(
                         val[0].get<float>(), val[1].get<float>(),
                         val[2].get<float>(), val[3].get<float>()
                     );
                 }
             } catch (...) {}
        }
    }
}

bool LoadPackFromJson(const std::string& packPath, const std::string& text) {
    json root;
    try {
        root = json::parse(text);
    } catch (const json::parse_error& e) {
        SetError(ShaderPackError::INVALID_FORMAT, FormatJsonError(text, e));
        return false;
    }

    g_runtime.pack.name = root.value("name", "ShaderPack");
    g_runtime.pack.enabled = root.value("enabled", true);

    if (root.contains("render_scale")) {
        g_runtime.renderScale = std::clamp(root.value("render_scale", 1.0f), 0.25f, 2.0f);
    }

    LoadShadowReceiver(packPath, root);
    LoadParamsFromJson(root);

    if (root.contains("passes") && root["passes"].is_array()) {
        for (const auto& item : root["passes"]) {
            LoadPassFromJson(packPath, item, g_runtime.pack.passes);
        }
    }

    if (g_runtime.pack.passes.empty()) {
        const std::array<std::pair<const char*, const char*>, 4> defaults = {
            std::make_pair("composite", "composite.spv"),
            std::make_pair("composite1", "composite1.spv"),
            std::make_pair("composite2", "composite2.spv"),
            std::make_pair("final", "final.spv"),
        };
        for (const auto& entry : defaults) {
            LoadDefaultPass(packPath, entry.first, entry.second, g_runtime.pack.passes);
        }
    }

    if (g_runtime.pack.passes.empty()) {
        SetError(ShaderPackError::SHADER_NOT_FOUND, "未找到可用的 ShaderPass");
        return false;
    }

    return true;
}

bool LoadPackFromFolder(const std::string& path) {
    std::string packPath = path;
    g_runtime.pack.path = packPath;

    std::string jsonText;
    const std::string jsonPath = JoinPath(packPath, "pack.json");
    if (LoadFileText(jsonPath, jsonText)) {
        return LoadPackFromJson(packPath, jsonText);
    }

    g_runtime.pack.name = "ShaderPack";
    g_runtime.pack.enabled = true;
    g_runtime.pack.passes.clear();

    const std::array<std::pair<const char*, const char*>, 4> defaults = {
        std::make_pair("composite", "composite.spv"),
        std::make_pair("composite1", "composite1.spv"),
        std::make_pair("composite2", "composite2.spv"),
        std::make_pair("final", "final.spv"),
    };

    for (const auto& entry : defaults) {
        LoadDefaultPass(packPath, entry.first, entry.second, g_runtime.pack.passes);
    }

    if (g_runtime.pack.passes.empty()) {
        SetError(ShaderPackError::NOT_FOUND, "未找到 pack.json 或默认 pass 文件");
        return false;
    }

    return true;
}

} // namespace
//=============================================================================
// ShaderPack API
//=============================================================================

WAR3_PACK_API ShaderPackError LoadShaderPack(const char* path) {
    if (!path || std::strlen(path) == 0) {
        SetError(ShaderPackError::NOT_FOUND, "ShaderPack 路径为空");
        return ShaderPackError::NOT_FOUND;
    }

    std::lock_guard<std::mutex> lock(g_runtime.mutex);
    if (!g_runtime.device) {
        SetError(ShaderPackError::INTERNAL_ERROR, "ShaderPack 运行时未初始化");
        return ShaderPackError::INTERNAL_ERROR;
    }

    ClearPack();
    g_runtime.lastErrorMessage.clear();
    g_runtime.pack.lastError = ShaderPackError::OK;

    if (!LoadPackFromFolder(path)) {
        g_runtime.pack.loaded = false;
        return g_runtime.pack.lastError;
    }

    g_runtime.pack.loaded = true;
    g_runtime.pack.lastError = ShaderPackError::OK;

    LogMessage(dxvk::str::format("ShaderPack: 已加载 ", g_runtime.pack.name, " (", g_runtime.pack.passes.size(), " passes)"));

    return ShaderPackError::OK;
}

WAR3_PACK_API ShaderPackError UnloadShaderPack() {
    std::lock_guard<std::mutex> lock(g_runtime.mutex);
    ClearPack();
    return ShaderPackError::OK;
}

WAR3_PACK_API ShaderPackError ReloadShaderPack() {
    std::lock_guard<std::mutex> lock(g_runtime.mutex);
    if (!g_runtime.pack.loaded) {
        SetError(ShaderPackError::NO_PACK_LOADED, "未加载 ShaderPack");
        return ShaderPackError::NO_PACK_LOADED;
    }

    const std::string path = g_runtime.pack.path;
    const bool enabled = g_runtime.pack.enabled;
    const auto params = g_runtime.params;
    const auto textures = g_runtime.textures;

    ClearPack();
    g_runtime.lastErrorMessage.clear();

    if (!LoadPackFromFolder(path)) {
        return g_runtime.pack.lastError;
    }

    g_runtime.pack.enabled = enabled;
    g_runtime.params = params;
    g_runtime.textures = textures;
    g_runtime.pack.loaded = true;

    LogMessage("ShaderPack: 热重载完成");
    return ShaderPackError::OK;
}

WAR3_PACK_API bool EnableShaderPack(bool enable) {
    std::lock_guard<std::mutex> lock(g_runtime.mutex);
    bool prev = g_runtime.pack.enabled;
    g_runtime.pack.enabled = enable;
    return prev;
}

WAR3_PACK_API bool IsShaderPackLoaded() {
    std::lock_guard<std::mutex> lock(g_runtime.mutex);
    return g_runtime.pack.loaded;
}

WAR3_PACK_API ShaderPackError GetShaderPackInfo(ShaderPackInfo* outInfo) {
    if (!outInfo)
        return ShaderPackError::INVALID_FORMAT;

    std::lock_guard<std::mutex> lock(g_runtime.mutex);
    std::memset(outInfo, 0, sizeof(ShaderPackInfo));

    if (!g_runtime.pack.loaded) {
        outInfo->lastError = g_runtime.pack.lastError;
        outInfo->flags = BuildPackFlags();
        return ShaderPackError::NO_PACK_LOADED;
    }

    std::snprintf(outInfo->name, sizeof(outInfo->name), "%s", g_runtime.pack.name.c_str());
    std::snprintf(outInfo->path, sizeof(outInfo->path), "%s", g_runtime.pack.path.c_str());
    outInfo->passCount = static_cast<uint32_t>(g_runtime.pack.passes.size());
    outInfo->flags = BuildPackFlags();
    outInfo->lastError = g_runtime.pack.lastError;
    return ShaderPackError::OK;
}

WAR3_PACK_API ShaderPackError SaveShaderPack() {
    std::lock_guard<std::mutex> lock(g_runtime.mutex);
    if (!g_runtime.pack.loaded) {
        return ShaderPackError::NO_PACK_LOADED;
    }

    // Try to read existing JSON to preserve comments/structure if possible?
    // Doing partial update with nlohmann::json will destroy formatting.
    // For now we just dump a new JSON which is standard behavior for tools.
    
    json root;
    // We try to read existing file first to respect other fields not managed by us?
    // But currently we manage most fields. Let's just create a new structure for consistency.
    
    root["name"] = g_runtime.pack.name;
    root["enabled"] = g_runtime.pack.enabled;
    root["render_scale"] = g_runtime.renderScale;
    root["shadow_receiver"] = g_runtime.pack.shadowReceiverSpirv.empty() ? "" : "shadow_receiver.spv"; // Approximation

    // Params
    json paramsObj = json::object();
    bool hasParams = false;
    for (uint32_t i = 0; i < SHADERPACK_MAX_PARAMS; ++i) {
        const auto& p = g_runtime.params[i];
        if (p.x != 0.0f || p.y != 0.0f || p.z != 0.0f || p.w != 0.0f) {
            paramsObj[std::to_string(i)] = {p.x, p.y, p.z, p.w};
            hasParams = true;
        }
    }
    if (hasParams) {
        root["params"] = paramsObj;
    }

    // Passes
    json passesArr = json::array();
    for (const auto& pass : g_runtime.pack.passes) {
        json passObj;
        passObj["name"] = pass.name;
        passObj["shader"] = pass.file;
        passObj["enabled"] = pass.enabled;
        passesArr.push_back(passObj);
    }
    root["passes"] = passesArr;

    std::string jsonPath = JoinPath(g_runtime.pack.path, "pack.json");
    std::ofstream file(jsonPath);
    if (file.is_open()) {
        file << root.dump(4);
        LogMessage("ShaderPack: 已保存配置到 " + jsonPath);
        return ShaderPackError::OK;
    } else {
        SetError(ShaderPackError::INTERNAL_ERROR, "无法写入 pack.json: " + jsonPath);
        return ShaderPackError::INTERNAL_ERROR;
    }
}

WAR3_PACK_API ShaderPackError SetTextureFromFile(TextureSlot slot, const char* path) {
    std::lock_guard<std::mutex> lock(g_runtime.mutex);
    if (!g_runtime.pack.loaded) {
        SetError(ShaderPackError::NO_PACK_LOADED, "未加载 ShaderPack");
        return ShaderPackError::NO_PACK_LOADED;
    }
    uint32_t index = static_cast<uint32_t>(slot);
    if (index >= SHADERPACK_MAX_TEXTURES) {
        SetError(ShaderPackError::INVALID_SLOT, "纹理槽无效");
        return ShaderPackError::INVALID_SLOT;
    }
    if (!path || std::strlen(path) == 0) {
        SetError(ShaderPackError::TEXTURE_NOT_FOUND, "纹理路径为空");
        return ShaderPackError::TEXTURE_NOT_FOUND;
    }

    std::string fullPath = JoinPath(g_runtime.pack.path, path);
    PendingTextureUpload upload = {};
    upload.slot = index;
    std::string errorMessage;
    ShaderPackError errorCode = ShaderPackError::OK;

    if (!DecodeImageRgba(fullPath, upload, errorCode, errorMessage) && fullPath != path) {
        if (!DecodeImageRgba(path, upload, errorCode, errorMessage)) {
            SetError(errorCode, errorMessage);
            return errorCode;
        }
    } else if (!errorMessage.empty() && errorCode != ShaderPackError::OK) {
        SetError(errorCode, errorMessage);
        return errorCode;
    }

    auto& pending = g_runtime.pendingUploads;
    pending.erase(
        std::remove_if(pending.begin(), pending.end(),
                       [index](const PendingTextureUpload& item) { return item.slot == index; }),
        pending.end());
    pending.push_back(std::move(upload));

    LogMessage(dxvk::str::format("ShaderPack: 纹理已排队上传 slot=", index, " path=", fullPath));
    return ShaderPackError::OK;
}

WAR3_PACK_API ShaderPackError ClearTexture(TextureSlot slot) {
    std::lock_guard<std::mutex> lock(g_runtime.mutex);
    uint32_t index = static_cast<uint32_t>(slot);
    if (index >= SHADERPACK_MAX_TEXTURES) {
        SetError(ShaderPackError::INVALID_SLOT, "纹理槽无效");
        return ShaderPackError::INVALID_SLOT;
    }
    g_runtime.textures[index] = nullptr;
    auto& pending = g_runtime.pendingUploads;
    pending.erase(
        std::remove_if(pending.begin(), pending.end(),
                       [index](const PendingTextureUpload& item) { return item.slot == index; }),
        pending.end());
    return ShaderPackError::OK;
}

WAR3_PACK_API ShaderPackError SetParamVec4(ParamSlot slot, float x, float y, float z, float w) {
    std::lock_guard<std::mutex> lock(g_runtime.mutex);
    uint32_t index = static_cast<uint32_t>(slot);
    if (index >= SHADERPACK_MAX_PARAMS) {
        SetError(ShaderPackError::INVALID_SLOT, "参数槽无效");
        return ShaderPackError::INVALID_SLOT;
    }
    g_runtime.params[index] = dxvk::Vector4(x, y, z, w);
    return ShaderPackError::OK;
}

WAR3_PACK_API ShaderPackError SetParamFloat(ParamSlot slot, float value) {
    return SetParamVec4(slot, value, 0.0f, 0.0f, 0.0f);
}

WAR3_PACK_API ShaderPackError GetParamVec4(ParamSlot slot, float* outX, float* outY, float* outZ, float* outW) {
    std::lock_guard<std::mutex> lock(g_runtime.mutex);
    uint32_t index = static_cast<uint32_t>(slot);
    if (index >= SHADERPACK_MAX_PARAMS) {
        SetError(ShaderPackError::INVALID_SLOT, "参数槽无效");
        return ShaderPackError::INVALID_SLOT;
    }
    if (outX) *outX = g_runtime.params[index].x;
    if (outY) *outY = g_runtime.params[index].y;
    if (outZ) *outZ = g_runtime.params[index].z;
    if (outW) *outW = g_runtime.params[index].w;
    return ShaderPackError::OK;
}

WAR3_PACK_API const char* GetLastShaderError() {
    std::lock_guard<std::mutex> lock(g_runtime.mutex);
    return g_runtime.lastErrorMessage.c_str();
}

WAR3_PACK_API const char* GetErrorString(ShaderPackError error) {
    switch (error) {
        case ShaderPackError::OK: return "OK";
        case ShaderPackError::NOT_FOUND: return "NOT_FOUND";
        case ShaderPackError::INVALID_FORMAT: return "INVALID_FORMAT";
        case ShaderPackError::SHADER_NOT_FOUND: return "SHADER_NOT_FOUND";
        case ShaderPackError::SHADER_COMPILE_ERROR: return "SHADER_COMPILE_ERROR";
        case ShaderPackError::TEXTURE_NOT_FOUND: return "TEXTURE_NOT_FOUND";
        case ShaderPackError::TEXTURE_LOAD_ERROR: return "TEXTURE_LOAD_ERROR";
        case ShaderPackError::TOO_MANY_PASSES: return "TOO_MANY_PASSES";
        case ShaderPackError::NO_PACK_LOADED: return "NO_PACK_LOADED";
        case ShaderPackError::INVALID_SLOT: return "INVALID_SLOT";
        case ShaderPackError::INTERNAL_ERROR: return "INTERNAL_ERROR";
        default: return "UNKNOWN";
    }
}

WAR3_PACK_API ShaderPackError SetRenderScale(float scale) {
    std::lock_guard<std::mutex> lock(g_runtime.mutex);
    g_runtime.renderScale = std::clamp(scale, 0.25f, 2.0f);
    return ShaderPackError::OK;
}

WAR3_PACK_API float GetRenderScale() {
    std::lock_guard<std::mutex> lock(g_runtime.mutex);
    return g_runtime.renderScale;
}

WAR3_PACK_API ShaderPackError SetPassEnabled(uint32_t passIndex, bool enable) {
    std::lock_guard<std::mutex> lock(g_runtime.mutex);
    if (passIndex >= g_runtime.pack.passes.size()) {
        SetError(ShaderPackError::INVALID_SLOT, "Pass 索引无效");
        return ShaderPackError::INVALID_SLOT;
    }
    g_runtime.pack.passes[passIndex].enabled = enable;
    return ShaderPackError::OK;
}

WAR3_PACK_API void SetShaderLogCallback(ShaderLogCallback callback, void* userData) {
    std::lock_guard<std::mutex> lock(g_runtime.mutex);
    g_runtime.logCallback = callback;
    g_runtime.logUserData = userData;
}

WAR3_PACK_API void GetShaderStats(uint32_t* outCompileCount, uint32_t* outErrorCount) {
    std::lock_guard<std::mutex> lock(g_runtime.mutex);
    if (outCompileCount)
        *outCompileCount = g_runtime.compileCount;
    if (outErrorCount)
        *outErrorCount = g_runtime.errorCount;
}

WAR3_PACK_API bool EnableShadowReceiverOverride(bool enable) {
    std::lock_guard<std::mutex> lock(g_runtime.mutex);
    const bool prev = g_runtime.shadowReceiverEnabled;
    g_runtime.shadowReceiverEnabled = enable;
    return prev;
}

WAR3_PACK_API bool IsShadowReceiverOverrideEnabled() {
    std::lock_guard<std::mutex> lock(g_runtime.mutex);
    return g_runtime.shadowReceiverEnabled;
}

//=============================================================================
// 内部接口
//=============================================================================

namespace internal {
void InitShaderPackRuntime(const dxvk::Rc<dxvk::DxvkDevice>& device) {
    std::lock_guard<std::mutex> lock(g_runtime.mutex);
    if (g_runtime.device)
        return;
    g_runtime.device = device;
    EnsureSampler();
    EnsureLayout();
    EnsureUboBuffer();
    EnsureFallbackTexture();
    LogMessage("ShaderPack: Vulkan 运行时初始化完成");
}

void RunShaderPackPasses(const dxvk::Rc<dxvk::DxvkCommandList>& ctx,
                         const dxvk::War3PipelineInput& input) {
    std::lock_guard<std::mutex> lock(g_runtime.mutex);
    if (!g_runtime.device)
        return;

    ProcessPendingUploads(ctx);

    if (!g_runtime.pack.loaded || !g_runtime.pack.enabled)
        return;
    if (g_runtime.pack.lastError != ShaderPackError::OK)
        return;
    if (!input.colorView)
        return;

    if (input.colorView->image()->info().sampleCount != VK_SAMPLE_COUNT_1_BIT) {
        static bool s_logged = false;
        if (!s_logged) {
            s_logged = true;
            Logger::warn("ShaderPack: 当前只支持 SampleCount=1，已跳过执行");
        }
        return;
    }

    EnsureSampler();
    EnsureLayout();
    EnsureUboBuffer();
    EnsureFallbackTexture();

    // [健壮性] 确保关键资源已正确初始化
    if (!g_runtime.samplerLinear) {
        static bool s_warned = false;
        if (!s_warned) {
            s_warned = true;
            Logger::err("ShaderPack: samplerLinear 未初始化，跳过执行");
        }
        return;
    }
    if (!g_runtime.fallbackView) {
        static bool s_warned = false;
        if (!s_warned) {
            s_warned = true;
            Logger::err("ShaderPack: fallbackView 未初始化，跳过执行");
        }
        return;
    }
    if (!g_runtime.uboBuffer) {
        static bool s_warned = false;
        if (!s_warned) {
            s_warned = true;
            Logger::err("ShaderPack: uboBuffer 未初始化，跳过执行");
        }
        return;
    }
    if (!g_runtime.layout) {
        static bool s_warned = false;
        if (!s_warned) {
            s_warned = true;
            Logger::err("ShaderPack: layout 未初始化，跳过执行");
        }
        return;
    }

    EnsureResources(input);

    if (!g_runtime.colorCopyView || !g_runtime.pingView || !g_runtime.pongView)
        return;

    CopyColorToInput(ctx, input.colorView);


    VkImageLayout depthOldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (input.depthView) {
        TransitionDepthToReadOnly(ctx, input.depthView, depthOldLayout);
    }

    auto now = std::chrono::steady_clock::now();
    float frameTime = 0.0f;
    if (!g_runtime.timeInitialized) {
        g_runtime.timeInitialized = true;
        g_runtime.lastFrameTime = now;
    } else {
        frameTime = std::chrono::duration<float>(now - g_runtime.lastFrameTime).count();
        g_runtime.lastFrameTime = now;
        g_runtime.totalTime += frameTime;
    }

    dxvk::Rc<dxvk::DxvkImageView> prevView = g_runtime.colorCopyView;
    dxvk::Rc<dxvk::DxvkImageView> currView = g_runtime.pingView;

    const VkFormat outFormat = g_runtime.cachedFormat;
    const VkExtent3D passExtent = g_runtime.cachedPassExtent;

    uint32_t activePasses = 0;
    for (auto& pass : g_runtime.pack.passes) {
        if (pass.enabled)
            activePasses++;
    }

    if (activePasses == 0)
        return;

    uint32_t passIndex = 0;
    for (auto& pass : g_runtime.pack.passes) {
        if (!pass.enabled)
            continue;

        // [诊断日志] 跟踪 pass 执行
        static uint32_t s_loggedPassCount = 0;
        if (s_loggedPassCount < 5) {
            s_loggedPassCount++;
            Logger::info(dxvk::str::format("ShaderPack: Executing pass[", passIndex, "] name=", pass.name));
        }

        const bool isLast = (passIndex + 1) >= activePasses;
        dxvk::Rc<dxvk::DxvkImageView> dstView = isLast ? input.colorView : currView;

        if (!EnsurePassPipeline(pass, outFormat)) {
            SetError(ShaderPackError::SHADER_COMPILE_ERROR, "ShaderPass pipeline 创建失败");
            g_runtime.pack.enabled = false;
            Logger::err("ShaderPack: Pipeline 创建失败，已禁用 pack（需重载）");
            return;
        }

        VkExtent3D extent = isLast ? input.colorView->mipLevelExtent(0u) : passExtent;
        UpdateUboOnCmd(ctx, input, extent, frameTime, g_runtime.totalTime);

        VkImageLayout originalLayout = dstView->getLayout();
        TransitionImageForRender(ctx, dstView, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

        VkRenderingAttachmentInfo attachment = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        attachment.imageView = dstView->handle();
        attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

        VkRenderingInfo renderInfo = { VK_STRUCTURE_TYPE_RENDERING_INFO };
        renderInfo.renderArea.offset = { 0, 0 };
        renderInfo.renderArea.extent = { extent.width, extent.height };
        renderInfo.layerCount = 1;
        renderInfo.colorAttachmentCount = 1;
        renderInfo.pColorAttachments = &attachment;

        ctx->cmdBeginRendering(&renderInfo);
        ctx->cmdBindPipeline(dxvk::DxvkCmdBuffer::ExecBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pass.pipeline);

        VkViewport viewport = { 0.0f, 0.0f, float(extent.width), float(extent.height), 0.0f, 1.0f };
        VkRect2D scissor = { { 0, 0 }, { extent.width, extent.height } };
        ctx->cmdSetViewport(1, &viewport);
        ctx->cmdSetScissor(1, &scissor);

        std::array<dxvk::DxvkDescriptorWrite, 8> descriptors = { };
        descriptors[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        descriptors[0].descriptor = nullptr;
        descriptors[0].buffer = g_runtime.uboBuffer->getSliceInfo(0u, sizeof(PackUBO));

        const auto colorDesc = g_runtime.colorCopyView ? g_runtime.colorCopyView->getDescriptor() : g_runtime.fallbackView->getDescriptor();
        const auto depthDesc = input.depthView ? input.depthView->getDescriptor() : g_runtime.fallbackView->getDescriptor();

        descriptors[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        descriptors[1].descriptor = colorDesc;

        descriptors[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        descriptors[2].descriptor = depthDesc;

        for (uint32_t i = 0; i < SHADERPACK_MAX_TEXTURES; i++) {
            descriptors[3 + i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            if (g_runtime.textures[i]) {
                descriptors[3 + i].descriptor = g_runtime.textures[i]->getDescriptor();
            } else {
                descriptors[3 + i].descriptor = g_runtime.fallbackView->getDescriptor();
            }
        }

        descriptors[7].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        descriptors[7].descriptor = prevView ? prevView->getDescriptor() : g_runtime.fallbackView->getDescriptor();

        PackPushConstants pc = { };
        const uint32_t samplerIndex = g_runtime.samplerLinear->getDescriptor().samplerIndex;
        for (uint32_t i = 0; i < 8; i++) {
            pc.samplerIndices[i] = samplerIndex;
        }
        for (uint32_t i = 0; i < SHADERPACK_MAX_PARAMS; i++) {
            pc.params[i] = g_runtime.params[i];
        }

        ctx->bindResources(dxvk::DxvkCmdBuffer::ExecBuffer,
                           g_runtime.layout,
                           descriptors.size(),
                           descriptors.data(),
                           sizeof(pc),
                           &pc);

        ctx->cmdDraw(3, 1, 0, 0);
        ctx->cmdEndRendering();

        if (!isLast) {
            TransitionImageToReadOnly(ctx, dstView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            prevView = dstView;
            currView = (currView == g_runtime.pingView) ? g_runtime.pongView : g_runtime.pingView;
        } else {
            TransitionImageToReadOnly(ctx, dstView, originalLayout);
        }

        ctx->track(dstView->image(), dxvk::DxvkAccess::Write);
        passIndex++;
    }

    if (input.depthView) {
        RestoreDepthLayout(ctx, input.depthView, depthOldLayout);
        ctx->track(input.depthView->image(), dxvk::DxvkAccess::Read);
    }
}

const std::vector<uint32_t>* GetShadowReceiverSpirv() {
    std::lock_guard<std::mutex> lock(g_runtime.mutex);
    if (!g_runtime.pack.loaded || !g_runtime.pack.enabled)
        return nullptr;
    if (!g_runtime.shadowReceiverEnabled)
        return nullptr;
    if (g_runtime.pack.shadowReceiverSpirv.empty())
        return nullptr;
    return &g_runtime.pack.shadowReceiverSpirv;
}

} // namespace internal

} // namespace war3shader
