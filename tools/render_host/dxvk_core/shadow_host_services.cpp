// The x86 data-layer and diagnostics entry points the shadow pass calls, as the
// x64 host provides them (see shadow_host_services.h). Every function here has
// its real definition in a src/d3d9/war3 file the host does not build; the set
// is exactly what the pass's objects leave undefined, and the link fails when
// the pass starts to need another one.
#include "shadow_host_services.h"

#include "../../../src/d3d9/d3d9_war3_shadow.h"
#include "../../../src/d3d9/war3/core/war3_runtime_profile.h"
#include "../../../src/d3d9/war3/hooks/war3_hook_widget_identity.h"
#include "../../../src/d3d9/war3/render/war3_render_objects.h"
#include "../../../src/d3d9/war3/render/war3_render_state.h"
#include "../../../src/d3d9/war3/render/war3_shadow_lifecycle.h"
#include "../../../src/d3d9/war3/render/war3_shadow_producer_policy.h"
#include "../../../src/d3d9/war3/render/war3_shadow_runtime_bridge.h"
#include "../../../src/d3d9/war3/shader/war3_shader_manager.h"
#include "../../../src/d3d9/war3/state/war3_render_state.h"
#include "../../../src/d3d9/war3/tools/war3_diagnostics_hub.h"
#include "../../../src/d3d9/war3/tools/war3_frame_evidence.h"
#include "../../../src/d3d9/war3/tools/war3_frame_inputs.h"
#include "../../../src/d3d9/war3/tools/war3_palette_object_evidence_sink.h"
#include "../../../src/d3d9/war3/tools/war3_perf_monitor.h"
#include "../../../src/d3d9/war3_shader_api.h"

#include <mutex>
#include <unordered_set>

namespace warvk::host::dxvkcore::shadowhost {
namespace {
std::mutex g_mutex;
DataLayerFacts g_facts;
thread_local CasterBoundsCounts g_presentedBounds;
}

void Publish(const DataLayerFacts& facts) noexcept {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_facts = facts;
}
void BeginFrame() noexcept {
  dxvk::war3::render::RenderObjectRegistry::instance().beginFrame();
}
DataLayerFacts Current() noexcept {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_facts;
}

void PublishPresentedBounds(const CasterBoundsCounts& counts) noexcept { g_presentedBounds = counts; }
CasterBoundsCounts PresentedBounds() noexcept { return g_presentedBounds; }

} // namespace warvk::host::dxvkcore::shadowhost

namespace host = warvk::host::dxvkcore::shadowhost;

// ---- facts of the x86 data layer, from the scene packet --------------------

namespace dxvk {

float War3RenderState::GetGameTime() { return host::Current().gameTime; }
void War3RenderState::SetTlsBatchHandle(uint32_t) { }

// ---- the unit outline's objects, from the Outline record --------------------

// The executor fills the set before a run (war3_shadow_scene_input.h
// ApplyOutlineHandles); the pass asks it about each caster's batchHandle. A
// handle is keyed as the x86 data layer keys it (war3_render_state.cpp
// NormalizeHandle): its low 20 bits, 0 being no handle.
namespace {
std::mutex g_outlineMutex;
std::unordered_set<uint32_t> g_outlineHandles;
uint32_t OutlineKey(uint32_t handle) {
  const uint32_t handleId = handle & 0x0FFFFFu;
  return handleId == 0u ? 0u : (handleId | 0x100000u);
}
}

void War3RenderState::AddOutlineHandle(uint32_t handle) {
  const uint32_t key = OutlineKey(handle);
  if (key == 0u)
    return;
  std::lock_guard<std::mutex> lock(g_outlineMutex);
  g_outlineHandles.insert(key);
}
void War3RenderState::ClearOutlineHandles() {
  std::lock_guard<std::mutex> lock(g_outlineMutex);
  g_outlineHandles.clear();
}
bool War3RenderState::HasOutlineHandles() {
  std::lock_guard<std::mutex> lock(g_outlineMutex);
  return !g_outlineHandles.empty();
}
bool War3RenderState::IsOutlineHandle(uint32_t handle) {
  const uint32_t key = OutlineKey(handle);
  if (key == 0u)
    return false;
  std::lock_guard<std::mutex> lock(g_outlineMutex);
  return g_outlineHandles.count(key) != 0u;
}
uint32_t War3RenderState::GetOutlineHandleCount() {
  std::lock_guard<std::mutex> lock(g_outlineMutex);
  return uint32_t(g_outlineHandles.size());
}

namespace war3::render {

uint64_t CurrentShadowCasterTombstoneSerial() { return host::Current().tombstoneSerial; }
uint64_t CurrentShadowStagePolicyRevision() { return host::Current().stagePolicyRevision; }

RenderObjectRegistry& RenderObjectRegistry::instance() {
  static RenderObjectRegistry registry;
  return registry;
}
// The pass reads only the frame number; the registry itself stays in x86.
void RenderObjectRegistry::beginFrame() { m_frameNumber.fetch_add(1u); }

} // namespace war3::render

namespace war3::runtime {

bool IsWar3RuntimeModuleEnabled(War3RuntimeModule module) {
  return (host::Current().runtimeDisabledModules & (1u << uint32_t(module))) == 0u;
}

} // namespace war3::runtime

namespace war3::state {

RenderState& RenderState::instance() {
  static RenderState state;
  return state;
}
uint32_t RenderState::getFrameIndex() const { return host::Current().frameIndex; }

} // namespace war3::state

namespace war3::hooks {

// Diagnostic naming of a caster in a log line; the widget table is x86's.
uint32_t QueryWidgetRawcodeByHandle(uint32_t) { return 0u; }

} // namespace war3::hooks

// ---- shader packs: no stage override reaches the host yet ------------------

namespace war3 {

ShaderManager& ShaderManager::get() {
  static ShaderManager manager;
  return manager;
}
bool ShaderManager::hasOverride(war3shader::RenderStageId) const { return false; }
War3Material* ShaderManager::getMaterial(war3shader::RenderStageId) { return nullptr; }

} // namespace war3
} // namespace dxvk

namespace war3shader::internal {

bool HasAnyRenderListeners() { return false; }
void DispatchRenderEvent(RenderEventID) { }

} // namespace war3shader::internal

// ---- diagnostics: nothing is recorded in the host --------------------------

namespace dxvk::war3 {

War3PerfMonitor::War3PerfMonitor() = default;
War3PerfMonitor::~War3PerfMonitor() = default;
War3PerfMonitor& War3PerfMonitor::instance() {
  static War3PerfMonitor monitor;
  return monitor;
}
War3PerfMonitor::ScopedSection::~ScopedSection() = default;
// The volumetric light pass (4d) times its CPU work as well.
War3PerfMonitor::ScopedCpuScope::~ScopedCpuScope() = default;
War3PerfMonitor::ScopedCpuScope War3PerfMonitor::cpuScope(const char*) { return { }; }
void War3PerfMonitor::setDevice(DxvkDevice*) { }
void War3PerfMonitor::shutdown() { }
War3PerfMonitor::ScopedSection War3PerfMonitor::scope(const char*, const Rc<DxvkCommandList>&) { return { }; }
void War3PerfMonitor::addCpuSample(const char*, double, const char*, uint32_t) { }
void War3PerfMonitor::noteShadowBudgetFrame(const War3ShadowCaptureStats&) { }
void War3PerfMonitor::noteShadowMapFallback(bool, bool) { }
void War3PerfMonitor::noteShadowReceiverFrame(uint32_t, uint64_t, uint32_t, uint32_t) { }
void War3PerfMonitor::noteShadowTaaFrame(const ShadowTaaFrameTelemetry&) { }
void War3PerfMonitor::notePointShadowPersistentFrame(const PointShadowPersistentFrameTelemetry&) { }

namespace tools {

void MarkInGameRenderReady(const char*, uint64_t) { }
void SetGpuFlightBreadcrumb(GpuFlightBreadcrumb, uint32_t, uint32_t, uint32_t) noexcept { }
void ResetGpuFlightCsmWork() noexcept { }
void SetGpuFlightCsmCascadeWork(uint32_t, uint32_t, uint64_t) noexcept { }
void ResetGpuFlightPointShadowWork(uint32_t) noexcept { }
void SetGpuFlightPointShadowFacePlan(uint32_t, uint32_t, uint32_t, uint32_t) noexcept { }
void SetGpuFlightPointShadowFaceWork(uint32_t, uint32_t, uint32_t, uint64_t) noexcept { }

namespace evidence {

bool InputsEnabled() noexcept { return false; }
bool PaletteObjectEvidenceEnabled() noexcept { return false; }
bool DrawsEnabled() noexcept { return false; }
uint64_t ActiveSession() noexcept { return 0u; }
uint64_t Record(uint64_t, Event) noexcept { return 0u; }
PaletteObjectEvidence& PaletteObjectRecorder() noexcept {
  static PaletteObjectEvidence recorder;
  return recorder;
}
// Never reached: InputsEnabled() is false.
std::shared_ptr<InputCapture> InputCapture::Create(const Rc<DxvkDevice>&) { return nullptr; }
uint64_t InputCapture::capture(const Rc<DxvkCommandList>&, Key, bool, uint64_t,
    const std::vector<const War3ShadowCasterDraw*>&, const std::vector<uint32_t>&, const void*, size_t,
    uint32_t, uint64_t, uint64_t, DxvkResourceBufferInfo) noexcept { return 0u; }

} // namespace evidence
} // namespace tools

namespace render {

void NoteShadowStageReplayPrepared(int) { }
void NoteShadowStageCascadeDrawn(int, uint32_t) { }
void NoteFinalShadowCasterFrame(const War3FrameScene&, const std::vector<const War3ShadowCasterDraw*>&, uint64_t) { }
void NoteShadowSceneTerminalStats(const War3ShadowCaptureStats&) { }

} // namespace render
} // namespace dxvk::war3
