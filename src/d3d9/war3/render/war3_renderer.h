// war3_renderer.h - War3 渲染逻辑入口（解耦 Hook 与渲染逻辑）

#pragma once

#include <cstdint>

namespace dxvk::war3::render {

class War3Renderer {
public:
    static War3Renderer& instance();

    // 帧生命周期
    void BeginFrame();
    void EndFrame();

    // 世界对象收集
    void OnWorldObjectsGroup(void* worldPtr, int groupIdx);

    // SceneNode 映射
    void OnWorldObjectEntry(void* worldObjectEntry, void* sceneNode);

    // 当前对象上下文（用于同步调试或特殊逻辑）
    void SetCurrentWorldObjectContext(void* worldObjectEntry, void* sceneNode);
    void ClearCurrentWorldObjectContext();
    void* GetCurrentWorldObjectEntry() const;
    void* GetCurrentSceneNode() const;

private:
    War3Renderer() = default;
};

} // namespace dxvk::war3::render
