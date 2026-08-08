// war3_renderer.h - War3 渲染逻辑入口（解耦 Hook 与渲染逻辑）

#pragma once

#include <cstddef>
#include <cstdint>

namespace dxvk::war3::render {

struct RenderObjectIdentitySnapshot;

class War3Renderer {
public:
    static War3Renderer& instance();

    // 帧生命周期
    void BeginFrame();
    void EndFrame();
    void ResetMapSession();
    void PublishSemanticRegistriesForScene();

    // 世界对象收集
    void OnWorldObjectsGroup(void* worldPtr, int groupIdx);

    // SceneNode 映射
    void OnWorldObjectEntry(void* worldObjectEntry, void* sceneNode);
    void OnVisibleRenderables(void* batchArray,
                              uint32_t before,
                              uint32_t after,
                              const RenderObjectIdentitySnapshot& identity);
    void OnTransparentRenderable(void* payload,
                                 uint32_t transparentType,
                                 uint32_t queueSlot,
                                 uint32_t sortKey,
                                 float distanceSq,
                                 const RenderObjectIdentitySnapshot& identity);

    // 当前对象上下文（用于同步调试或特殊逻辑）
    void SetCurrentWorldObjectContext(void* worldObjectEntry, void* sceneNode);
    void ClearCurrentWorldObjectContext();
    void* GetCurrentWorldObjectEntry() const;
    void* GetCurrentSceneNode() const;

private:
    War3Renderer() = default;

    uint32_t m_semanticEndFrameBuildAttemptsThisFrame = 0;
    bool m_semanticEndFrameSawSkinnedThisFrame = false;
    uint64_t m_rendererFrameSerial = 0;
    uint64_t m_lastSemanticRegistryPublishFrameSerial = 0;
    uint64_t m_lastSemanticEndFrameFlushFrameSerial = 0;
    uint64_t m_lastSemanticEndFrameFlushPublishRevision = 0;
};

} // namespace dxvk::war3::render
