// war3_renderer.cpp - War3 渲染逻辑入口实现

#include "war3_renderer.h"
#include "../model/war3_model_registry.h"
#include "war3_render_objects.h"
#include "../render/war3_shadow_object_registry.h"
#include "war3_scene_collector.h"

namespace dxvk::war3::render {

namespace {
thread_local void* s_tlsWorldObjectEntry = nullptr;
thread_local void* s_tlsSceneNode = nullptr;
} // namespace

War3Renderer& War3Renderer::instance() {
    static War3Renderer* s_instance = new War3Renderer();
    return *s_instance;
}

void War3Renderer::BeginFrame() {
    RenderObjectRegistry::instance().beginFrame();
    model::ModelRegistry::instance().beginFrame();
    model::ModelInstanceRegistry::instance().beginFrame();
    model::PoseRegistry::instance().beginFrame();
    ShadowObjectRegistry::instance().beginFrame();
}

void War3Renderer::EndFrame() {
    RenderObjectRegistry::instance().endFrame();
    model::ModelRegistry::instance().endFrame();
    model::ModelInstanceRegistry::instance().endFrame();
    model::PoseRegistry::instance().endFrame();
    ShadowObjectRegistry::instance().endFrame();
}

void War3Renderer::OnWorldObjectsGroup(void* worldPtr, int groupIdx) {
    SceneCollector::CollectWorldObjects(worldPtr, groupIdx);
}

void War3Renderer::OnWorldObjectEntry(void* worldObjectEntry, void* sceneNode) {
    RenderObjectRegistry::instance().mapSceneNode(worldObjectEntry, sceneNode);
}

void War3Renderer::SetCurrentWorldObjectContext(void* worldObjectEntry, void* sceneNode) {
    s_tlsWorldObjectEntry = worldObjectEntry;
    s_tlsSceneNode = sceneNode;
}

void War3Renderer::ClearCurrentWorldObjectContext() {
    s_tlsWorldObjectEntry = nullptr;
    s_tlsSceneNode = nullptr;
}

void* War3Renderer::GetCurrentWorldObjectEntry() const {
    return s_tlsWorldObjectEntry;
}

void* War3Renderer::GetCurrentSceneNode() const {
    return s_tlsSceneNode;
}

} // namespace dxvk::war3::render
