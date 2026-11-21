// war3_proxy_frame.cpp - see war3_proxy_frame.h

#define WAR3_SHADER_API_INTERNAL

#include "war3_proxy_frame.h"

#include "war3_player_settings.h"
#include "war3_proxy_casters.h"
#include "war3_proxy_diag.h"
#include "war3_proxy_mode.h"
#include "war3_proxy_status.h"
#include "../core/war3_internal_test_config.h"
#include "../core/war3_runtime_profile.h"
#include "../effects/war3_effect_scene_input.h"
#include "../effects/war3_shadow_scene_input.h"
#include "../effects/war3_sun_cycle.h"
#include "../render/war3_render_state.h"
#include "../render/war3_settings_resolver.h"
#include "../render/war3_shadow_lifecycle.h"
#include "../render/war3_weather_runtime.h"
#include "../state/war3_fog_volume.h"
#include "../tools/war3_diagnostics_hub.h"
#include "../../d3d9_war3_light.h"
#include "../../d3d9_war3_pipeline.h"
#include "../../d3d9_war3_scene.h"
#include "../../war3_shader_api.h"
#include "../../../util/log/log.h"
#include "../../../util/util_string.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>

namespace dxvk::war3::proxy {

namespace {

namespace scene = effects::scene;

struct State {
  bool connected = false;
  // Inside the game's world RenderScene, whether or not markers are sent.
  bool inWorldScene = false;
  // The host's view of the world scene, kept here as well: a marker the host
  // would refuse is never sent.
  scene::SceneTracker tracker;
  uint64_t draws = 0;
  uint64_t frame = 1;

  // The world camera of this frame, by the device's capture rule: the
  // perspective projection of the best world stage, largest viewport.
  bool haveCamera = false;
  uint32_t cameraTier = 0;
  uint64_t cameraArea = 0;
  scene::Camera camera;
  bool haveLastGood = false;
  uint64_t lastGoodFrame = 0;
  scene::Camera lastGood;

  // Render-owner copy of the settings, resolved at each frame boundary.
  War3RenderSettings settings;
  War3SettingsFrameResolver resolver;
  // The mailbox revision `settings` was taken at.
  uint64_t settingsRevision = 0;

  // The frame's sun as the shadow pass resolves it. In process the pass feeds
  // it back into the settings for the next frame's world lighting; the host's
  // pass cannot, so the same resolution runs here.
  War3SunCycle sunCycle;
  bool haveSun = false;
  War3SunCycle::Result sun;

  FrameStatus status;
  bool loggedFirstInsertion = false;

  // The last frame's caster account, for the diagnostics block.
  CasterFrameAccount lastCasters;

  // The world lighting the front end holds (SendWorldLighting), whether its
  // white material ambient and ambient override are on for the draws now
  // (EmissiveOnlyDraw / WorldDrawLighting), and whether it is set aside for a
  // camera-space overlay.
  WarVKD3D9WorldLighting worldLighting = {};
  bool worldLightingHeld = false;
  uint32_t ambientWhiteHeld = 0u;
  uint32_t ambientOverrideHeld = 0u;
  uint64_t emissiveOnlyDraws = 0u;
  uint64_t fogTintedDraws = 0u;
  uint64_t colorSourceDraws = 0u;
  bool overlayHeld = false;
  uint64_t overlayDraws = 0u;
};

State g;
std::mutex g_statusMutex;
FrameStatus g_published;

// The settings of proxy mode start as War3RenderPipeline's constructor leaves
// them: defaults, the player's saved settings, the DXVK_WAR3_* overrides.
// Not yet here: the VRAM preset (the adapter is the host's) and the saved
// "FPS unlocked" switch (it belongs to the overlay).
std::shared_ptr<War3RenderSettingsMailbox> CreateMailbox() {
  War3RenderSettings settings;
  {
    war3::player::Extras extras;
    extras.nativeShadowMode = War3RenderState::GetNativeShadowMode();
    const auto report = war3::player::ApplyAtStartup(settings, extras);
    War3RenderState::SetNativeShadowMode(extras.nativeShadowMode);
    if (report.applied != 0u || report.unknown != 0u || !report.rejected.empty()) {
      Logger::info(str::format("WarVK player settings: applied=", report.applied,
                               " unknown=", report.unknown, " rejected=", report.rejected.size()));
    }
  }
  ApplyWar3SettingsEnvironmentOverrides(settings);

  auto mailbox = std::make_shared<War3RenderSettingsMailbox>();
  mailbox->pending = settings;
  mailbox->pendingRevision = 1u;
  mailbox->appliedRevision = 1u;
  g.settings = settings;
  return mailbox;
}

void Publish() {
  std::lock_guard<std::mutex> lock(g_statusMutex);
  g_published = g.status;
  g_published.casters = CastersStatus();
}

// The status file the gates poll (WarVK/Temp/runtime_status.json): in process
// the pipeline refreshes it every 1200 frames; here about once a second, so a
// slow proxy-mode frame rate still keeps it as fresh as the gates require.
void ExportStatusPeriodically() {
  static std::chrono::steady_clock::time_point s_last;
  const auto now = std::chrono::steady_clock::now();
  if (now - s_last < std::chrono::milliseconds(1000))
    return;
  s_last = now;
  tools::ExportRuntimeStatusSnapshot("proxy-periodic", g.frame);
}

// One refusal means the two ends no longer agree on the frame: stop sending
// instead of guessing. The game's own picture is unaffected.
bool Sent(HRESULT hr, const char* what) {
  if (SUCCEEDED(hr))
    return true;
  ++g.status.refused;
  if (!g.status.sendingStopped) {
    g.status.sendingStopped = true;
    Logger::warn(str::format("WarVK proxy mode: ", what, " refused hr=0x", std::hex, uint32_t(hr),
                             std::dec, "; no further records are sent"));
  }
  return false;
}

bool Sending() { return g.connected && !g.status.sendingStopped; }

bool Mark(scene::Stage stage, const char* what) {
  return Sent(MarkStage(uint32_t(stage)), what);
}

template <typename Bytes>
bool Record(uint32_t schema, const Bytes& bytes, uint32_t used, const char* what) {
  if (!Sent(SetFrameScene(schema, bytes.data(), used), what))
    return false;
  ++g.status.records;
  g.status.lastRecordMask |= 1u << schema;
  return true;
}

template <typename Bytes>
bool Record(uint32_t schema, const Bytes& bytes, const char* what) {
  return Record(schema, bytes, uint32_t(bytes.size()), what);
}

// D3D9DeviceEx::War3MaybeInsertBeforeUi, the camera capture: only a world
// stage that draws with the main camera may set it.
void SampleCamera(IDirect3DDevice9* device) {
  if (!War3RenderState::HasWorldStageThisFrame() ||
      War3RenderState::CurrentLayer() == War3RenderLayer::UI)
    return;
  const auto category = War3RenderState::GetStageCategory();
  const int stage = War3RenderState::GetStage();
  uint32_t tier = 0u;
  if (category == War3RenderState::StageCategory::WorldObject) {
    tier = (stage >= 11 && stage <= 13) ? 3u : 0u; // units, buildings, destructibles
  } else if (category == War3RenderState::StageCategory::Terrain) {
    tier = stage == 1 ? 3u : stage == 10 ? 2u : 0u; // main terrain; doodads and trees
  }
  if (tier == 0u || tier < g.cameraTier)
    return;

  D3DVIEWPORT9 viewport = {};
  D3DMATRIX view = {}, projection = {};
  if (FAILED(device->GetViewport(&viewport)) ||
      FAILED(device->GetTransform(D3DTS_PROJECTION, &projection)))
    return;
  const uint64_t area = uint64_t(viewport.Width) * uint64_t(viewport.Height);
  const bool perspective = std::abs(std::abs(projection.m[2][3]) - 1.0f) <= 1e-3f &&
                           std::abs(projection.m[3][3]) <= 1e-3f;
  if (!perspective || (tier == g.cameraTier && area < g.cameraArea) ||
      FAILED(device->GetTransform(D3DTS_VIEW, &view)))
    return;

  g.cameraTier = tier;
  g.cameraArea = std::max(g.cameraArea, area);
  scene::Camera& camera = g.camera;
  std::memcpy(camera.view.data(), &view, sizeof(view));
  std::memcpy(camera.projection.data(), &projection, sizeof(projection));
  camera.x = viewport.X;
  camera.y = viewport.Y;
  camera.width = viewport.Width;
  camera.height = viewport.Height;
  camera.minZ = viewport.MinZ;
  camera.maxZ = viewport.MaxZ;
  camera.frame = uint32_t(g.frame);
  const float gameTime = War3RenderState::GetGameTime();
  camera.gameTime = std::isfinite(gameTime) ? gameTime : -1.0f;
  g.haveCamera = true;
  g.haveLastGood = true;
  g.lastGoodFrame = g.frame;
  g.lastGood = camera;

  if (!tools::IsInGameRenderReady())
    tools::MarkInGameRenderReady("ProxyMode/WorldCamera", g.frame);
}

// What War3RenderPipeline::OnFrameStart decides for the frame, from the same
// settings and switches.
struct Wants {
  bool shadowReceiver = false; // the shadow pass has work: sun shadows, outline or point lights
  bool outline = false;
  bool postFx = false;
  bool ssao = false;
  bool aa = false;
  bool snow = false;
  bool any = false;
  war3::render::WeatherFrameSnapshot weather;
};

Wants Decide(const War3RenderSettings& settings) {
  Wants wants;
  if constexpr (!internal::kWar3RenderModuleTakeoverEnabled)
    return wants;
  using runtime::IsWar3RuntimeModuleEnabled;
  using runtime::War3RuntimeModule;
  const bool moduleShadowMap = IsWar3RuntimeModuleEnabled(War3RuntimeModule::ShadowMap);
  const bool moduleShadowReceiver = IsWar3RuntimeModuleEnabled(War3RuntimeModule::ShadowReceiver);
  const bool modulePostFx = IsWar3RuntimeModuleEnabled(War3RuntimeModule::PostFx);

  const bool disableWar3Shadow = internal::kNativeShadowDisableWar3ShadowReceiverWhenMode1 &&
                                 War3RenderState::GetNativeShadowMode() >= 1u;
  wants.outline = moduleShadowReceiver && settings.occludedOutline.enabled &&
                  War3RenderState::HasOutlineHandles() &&
                  !(disableWar3Shadow && internal::kNativeShadowDisableOutlineWhenMode1);
  const bool shadows = moduleShadowMap && moduleShadowReceiver && settings.shadows.enabled &&
                       !disableWar3Shadow;
  const bool pointLights = moduleShadowReceiver && settings.shadows.pointLightsEnabled &&
                           War3LightManager::Instance().HasActiveLights();
  wants.postFx = modulePostFx && settings.postFx.enabled &&
                 !war3shader::internal::IsNativePostProcessDisabled();
  wants.ssao = wants.postFx && settings.postFx.ssao.enabled &&
               IsWar3RuntimeModuleEnabled(War3RuntimeModule::Ssao);
  wants.aa = wants.postFx && settings.postFx.aa.mode != War3AAMode::None &&
             IsWar3RuntimeModuleEnabled(War3RuntimeModule::Aa);

  auto& weather = war3::render::WeatherRuntime::instance();
  if (weather.hasEnabled()) {
    wants.weather = weather.snapshot(g.frame, g.status.sessionGeneration + 1u, 1u);
    wants.snow = wants.weather.enabled && wants.weather.id != 0;
  }

  const bool beforeUi = shadows || wants.outline || pointLights || wants.postFx || wants.snow;
  // In process the shadow pass is part of every BeforeUi insertion: it also
  // resolves the frame's sun for the passes after it.
  wants.shadowReceiver = beforeUi && moduleShadowReceiver;
  wants.any = beforeUi;
  return wants;
}

bool SendShadowRecords(const War3RenderSettings& settings, const Wants& wants) {
  if (!Record(scene::LightingSchema, scene::EncodeLighting(effects::LightingFromSettings(settings)),
              "Lighting record"))
    return false;

  if (settings.shadows.pointLightsEnabled && War3LightManager::Instance().HasActiveLights()) {
    scene::PointLights lights;
    for (const War3PointLight& light : War3LightManager::Instance().GetActiveLights()) {
      if (!light.active || lights.count >= scene::PointLightsMax)
        continue;
      scene::PointLight& out = lights.lights[lights.count++];
      out.position = { light.position.x, light.position.y, light.position.z };
      out.range = std::max(1.0f, light.position.w);
      out.color = { light.color.x, light.color.y, light.color.z };
      out.intensity = light.color.w;
      out.shadowIntensity = light.params.x;
    }
    std::array<uint8_t, scene::PointLightsMaxBytes> bytes;
    const uint32_t used = scene::EncodePointLights(lights, bytes);
    if (lights.count != 0u && !Record(scene::PointLightsSchema, bytes, used, "PointLights record"))
      return false;
  }

  if (wants.outline) {
    scene::Outline outline;
    const auto& authored = settings.occludedOutline;
    outline.enabled = authored.enabled ? 1u : 0u;
    outline.mode = uint32_t(authored.mode);
    outline.useScreenSpace = authored.useScreenSpace ? 1u : 0u;
    outline.widthPx = authored.widthPx;
    outline.showVisible = authored.showVisible ? 1u : 0u;
    outline.showOccluded = authored.showOccluded ? 1u : 0u;
    outline.color = { authored.colorR, authored.colorG, authored.colorB, authored.colorA };
    outline.handleCount =
        War3RenderState::CopyOutlineHandles(outline.handles.data(), scene::OutlineHandlesMax);
    static std::array<uint8_t, scene::OutlineMaxBytes> bytes;
    const uint32_t used = scene::EncodeOutline(outline, bytes);
    if (outline.handleCount != 0u && !Record(scene::OutlineSchema, bytes, used, "Outline record"))
      return false;
  }

  if (settings.postFx.volumetricLight.enabled &&
      !Record(scene::VolumetricLightSchema,
              scene::EncodeVolumetricLight(effects::VolumetricFromSettings(settings.postFx.volumetricLight)),
              "VolumetricLight record"))
    return false;

  if (War3FogVolumeManager::Instance().HasActiveVolumes()) {
    std::array<War3FogVolumeAuthored, War3FogVolumeFrameSnapshot::kMaxVolumes> authored;
    scene::FogVolumes volumes;
    volumes.count = std::min(War3FogVolumeManager::Instance().ListFrameVolumesAuthored(authored),
                             scene::FogVolumesMax);
    for (uint32_t i = 0; i < volumes.count; ++i) {
      scene::FogVolume& out = volumes.volumes[i];
      out.shape = uint32_t(authored[i].shape);
      out.active = 1u;
      out.position = { authored[i].position.x, authored[i].position.y, authored[i].position.z };
      out.halfSize = { authored[i].halfSize.x, authored[i].halfSize.y, authored[i].halfSize.z };
      out.rotationDegrees = { authored[i].rotationDegrees.x, authored[i].rotationDegrees.y,
                              authored[i].rotationDegrees.z };
      out.density = authored[i].density;
      out.edgeFeather = authored[i].edgeFeather;
    }
    std::array<uint8_t, scene::FogVolumesMaxBytes> bytes;
    const uint32_t used = scene::EncodeFogVolumes(volumes, bytes);
    if (volumes.count != 0u && !Record(scene::FogVolumesSchema, bytes, used, "FogVolumes record"))
      return false;
  }

  // Last of the shadow records: it closes the frame's caster account.
  scene::ShadowFrame frame;
  const float timeOfDay = War3RenderState::GetGameTime();
  frame.timeOfDay = std::isfinite(timeOfDay) ? timeOfDay : -1.0f;
  frame.runtimeDisabledModules = runtime::GetWar3RuntimeConfig().effectiveDisabledMask;
  frame.tombstoneSerial = war3::render::CurrentShadowCasterTombstoneSerial();
  frame.stagePolicyRevision = war3::render::CurrentShadowStagePolicyRevision();
  // The casters are the draws since the last insertion that carry a
  // ShadowCaster record (war3_proxy_casters.cpp).
  const CasterFrameAccount casters = CastersFrameAccount();
  g.lastCasters = casters;
  frame.casters = casters.casters;
  frame.omittedCasters = casters.omitted;
  frame.omissionReasons = casters.omissionReasons;
  frame.dynamicSkinnedOutputCount = casters.dynamicCasters;
  frame.dynamicPoseSignature = casters.dynamicSignature;
  return Record(scene::ShadowFrameSchema, scene::EncodeShadowFrame(frame), "ShadowFrame record");
}

// The world scene is complete and nothing has been drawn after it: the
// frame's records, then the marker the host runs the effects at.
void InsertBeforeUi(bool atPresent) {
  // Once per completed scene, whatever comes of it.
  g.tracker.insertedBeforeUi();
  // As the device's boundary: a frame whose world stages never ran has no
  // world to put the effects on.
  if (!War3RenderState::HasWorldStageThisFrame())
    return;
  const Wants wants = Decide(g.settings);
  if (!wants.any)
    return;

  // The device's rule: this frame's camera, else the previous frame's
  // (kWar3WorldCameraFallbackMaxAgeFrames), else the effects wait.
  const scene::Camera* camera = nullptr;
  if (g.haveCamera) {
    camera = &g.camera;
  } else if (g.haveLastGood && g.frame - g.lastGoodFrame <= kWar3WorldCameraFallbackMaxAgeFrames) {
    camera = &g.lastGood;
    ++g.status.cameraFallbacks;
  }
  if (camera == nullptr) {
    ++g.status.framesWithoutCamera;
    return;
  }

  g.status.lastRecordMask = 0u;
  if (!Record(scene::CameraSchema, scene::EncodeCamera(*camera), "Camera record"))
    return;
  if (wants.shadowReceiver && !SendShadowRecords(g.settings, wants))
    return;
  // The pass resolves the sun whenever it has shadows or an outline to draw.
  if (wants.shadowReceiver && (g.settings.shadows.enabled || g.settings.occludedOutline.enabled)) {
    g.sun = g.sunCycle.Resolve(&g.settings, War3RenderState::GetGameTime(),
                               std::chrono::steady_clock::now());
    g.haveSun = true;
  }
  if (wants.ssao && !Record(scene::SsaoSchema,
                            scene::EncodeSsao(effects::SsaoFromSettings(g.settings.postFx.ssao)),
                            "Ssao record"))
    return;
  if (wants.snow && !Record(scene::SnowSchema, scene::EncodeSnow(effects::SnowFromWeather(wants.weather)),
                            "Snow record"))
    return;
  if (wants.aa && !Record(scene::AaSchema, scene::EncodeAa(effects::AaFromSettings(g.settings.postFx.aa)),
                          "Aa record"))
    return;
  if (wants.postFx && !Record(scene::BloomSchema,
                              scene::EncodeBloom(effects::BloomFromSettings(g.settings.postFx)),
                              "Bloom record"))
    return;
  if (!Mark(scene::Stage::BeforeUi, "BeforeUi marker"))
    return;
  // The host's pass took the casters sent so far, or dropped them.
  CastersBeginFrame(g.settings);
  ++g.status.insertions;
  if (atPresent)
    ++g.status.insertionsAtPresent;
  if (!g.loggedFirstInsertion) {
    g.loggedFirstInsertion = true;
    Logger::info(str::format("WarVK proxy mode: first BeforeUi frame=", g.frame,
                             " records=0x", std::hex, g.status.lastRecordMask, std::dec,
                             " camera=", camera->width, "x", camera->height,
                             atPresent ? " at Present" : " before the first draw after the world"));
  }
}

// War3ShadowReceiverPass::Run's feedback of the resolved sun: into the pending
// settings, unless the author edited them since the frame took its copy. The
// direction is the one the world is shaded from, not the shadow's.
void FeedSunBack(War3RenderSettingsMailbox& mailbox) {
  if (!g.haveSun)
    return;
  g.haveSun = false;
  std::lock_guard<std::mutex> lock(mailbox.mutex);
  if (mailbox.pendingRevision != g.settingsRevision) {
    // An edit since the frame took its copy (the open panel commits every
    // frame): what it wrote stays; the outputs of the cycles that are still
    // running follow the sun, or the world light would wait for the panel
    // to close.
    auto& pending = mailbox.pending;
    if (g.settings.dayNight.clockMode == War3LightingClockMode::GameTime &&
        pending.dayNight.clockMode == War3LightingClockMode::GameTime)
      pending.dayNight.renderTimeHours = g.sun.renderTimeHours;
    if (g.settings.dayNight.celestialMotionEnabled && pending.dayNight.celestialMotionEnabled)
      pending.sun.direction = g.sun.lightDirection;
    if (g.settings.dayNight.timeColorGradingEnabled && pending.dayNight.timeColorGradingEnabled)
      pending.sun.color = g.sun.color;
    ++mailbox.pendingRevision;
    return;
  }
  mailbox.pending.dayNight.renderTimeHours = g.sun.renderTimeHours;
  if (g.settings.dayNight.celestialMotionEnabled)
    mailbox.pending.sun.direction = g.sun.lightDirection;
  if (g.settings.dayNight.timeColorGradingEnabled)
    mailbox.pending.sun.color = g.sun.color;
  ++mailbox.pendingRevision;
}

void ResolveSettings() {
  const auto mailbox = FrameSettingsMailbox();
  FeedSunBack(*mailbox);
  g.resolver.BeginFrame(*mailbox, g.settings);
  g.resolver.FinishFrame(*mailbox, g.settings);
  static warvk::diag::CasterSettingsView view;
  view.open(GetCurrentProcessId());
  warvk::diag::CasterWords words;
  for (size_t i = 0; i < words.size(); ++i) {
    const auto& cls = g.settings.casterClasses[i];
    words[i] = warvk::diag::CasterWord(cls.castShadow, cls.maxShadowLength);
  }
  view.publish(words);
  view.publishSun(warvk::diag::EncodeSunStyle(g.settings.sunStyle));
  view.publishWater(warvk::diag::EncodeWater(g.settings.water));
  std::lock_guard<std::mutex> lock(mailbox->mutex);
  g.settingsRevision = mailbox->appliedRevision;
}

void LogCasters() {
  const CasterStatus& casters = CastersStatus();
  std::string skipped;
  for (uint64_t count : casters.skipped)
    skipped += str::format(skipped.empty() ? "" : ",", count);
  Logger::info(str::format("WarVK proxy mode: casters frame=", g.status.frames,
                           " considered=", casters.considered,
                           " sent=", casters.sent, " alpha=", casters.alpha,
                           " terrain=", casters.terrain, " doodads=", casters.doodads,
                           " worldObjects=", casters.worldObjects, " units=", casters.units,
                           " water=", casters.water,
                           " dynamicBytes=", casters.dynamicBytes,
                           " omittedFrames=", casters.omittedFrames, " skipped=[", skipped, "]",
                           " withHandle=", casters.withHandle, " unitsWithHandle=", casters.unitsWithHandle,
                           " outlineMatched=", casters.outlineMatched));
}

// D3D9DeviceEx::War3EffectiveLight0 / War3EffectiveMaterial /
// War3EffectiveAmbient as values for the front end, from the frame's settings.
void SendWorldLighting(bool active) {
  g.overlayHeld = false;
  if (!active) {
    SetWorldLighting(nullptr);
    g.worldLightingHeld = false;
    return;
  }
  WarVKD3D9WorldLighting lighting = {};
  lighting.size = sizeof(lighting);
  lighting.active = 1u;
  lighting.materialAmbientWhite = 1u;
  const auto& sun = g.settings.sun;
  const auto& ambient = g.settings.ambient;
  const auto finite3 = [](const Vector4& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
  };
  if (!sun.enabled) {
    lighting.sunMode = 1u;
  } else if ((sun.direction.x != 0.0f || sun.direction.y != 0.0f || sun.direction.z != 0.0f) &&
             finite3(sun.direction) && finite3(sun.color) && std::isfinite(sun.intensity)) {
    lighting.sunMode = 2u;
    const float r = sun.color.x * sun.intensity;
    const float g_ = sun.color.y * sun.intensity;
    const float b = sun.color.z * sun.intensity;
    lighting.sunDirection[0] = sun.direction.x;
    lighting.sunDirection[1] = sun.direction.y;
    lighting.sunDirection[2] = sun.direction.z;
    lighting.sunColor[0] = r;
    lighting.sunColor[1] = g_;
    lighting.sunColor[2] = b;
    // A fifth of the direct light as skylight, raised to the ambient floor.
    lighting.sunAmbient[0] = r * 0.2f;
    lighting.sunAmbient[1] = g_ * 0.2f;
    lighting.sunAmbient[2] = b * 0.2f;
    if (ambient.overrideEnabled && finite3(ambient.color)) {
      lighting.sunAmbient[0] = std::max(lighting.sunAmbient[0], ambient.color.x);
      lighting.sunAmbient[1] = std::max(lighting.sunAmbient[1], ambient.color.y);
      lighting.sunAmbient[2] = std::max(lighting.sunAmbient[2], ambient.color.z);
    }
  }
  if (ambient.overrideEnabled && finite3(ambient.color)) {
    lighting.ambientOverride = 1u;
    lighting.ambientColor = D3DCOLOR_ARGB(
        0xFF,
        static_cast<int>(std::max(0.0f, std::min(1.0f, ambient.color.x)) * 255.0f),
        static_cast<int>(std::max(0.0f, std::min(1.0f, ambient.color.y)) * 255.0f),
        static_cast<int>(std::max(0.0f, std::min(1.0f, ambient.color.z)) * 255.0f));
  }
  const HRESULT hr = SetWorldLighting(&lighting);
  g.worldLighting = lighting;
  g.worldLightingHeld = hr == S_OK;
  g.ambientWhiteHeld = lighting.materialAmbientWhite;
  g.ambientOverrideHeld = lighting.ambientOverride;
  static bool s_logged = false;
  if (!s_logged) {
    s_logged = true;
    Logger::info(str::format("WarVK proxy mode: world lighting sunMode=", lighting.sunMode,
                             " ambientOverride=", lighting.ambientOverride,
                             " hr=0x", std::hex, uint32_t(hr), std::dec));
  }
}

} // namespace

std::shared_ptr<War3RenderSettingsMailbox> FrameSettingsMailbox() {
  static const std::shared_ptr<War3RenderSettingsMailbox> mailbox = CreateMailbox();
  return mailbox;
}

void FrameConnect() {
  DiagConnect();
  FrameSettingsMailbox();
  g.connected = true;
  g.inWorldScene = false;
  g.tracker = scene::SceneTracker();
  g.tracker.generation = g.status.sessionGeneration;
  g.draws = 0;
  g.haveCamera = g.haveLastGood = false;
  g.cameraTier = 0u;
  g.cameraArea = 0u;
  g.status.sendingStopped = false;
  ResolveSettings();
  CastersBeginFrame(g.settings);
  Publish();
}

void FrameDisconnect() {
  if (g.connected) {
    Logger::info(str::format("WarVK proxy mode: frames=", g.status.frames,
                             " worldScenes=", g.status.worldScenes,
                             " insertions=", g.status.insertions,
                             " atPresent=", g.status.insertionsAtPresent,
                             " noCamera=", g.status.framesWithoutCamera,
                             " cameraFallbacks=", g.status.cameraFallbacks,
                             " records=", g.status.records, " refused=", g.status.refused));
    LogCasters();
  }
  CastersEndFrame();
  g.connected = false;
  g.inWorldScene = false;
  g.haveSun = false;
  Publish();
}

void FrameWorldScene(bool entered) {
  if (!g.connected)
    return;
  g.inWorldScene = entered;
  // The world lighting is the front end's own business: it holds whether or
  // not records are sent.
  SendWorldLighting(entered);
  if (!Sending())
    return;
  if (entered) {
    // A scene inside a scene is the game's own business: one bracket.
    if (g.tracker.active || !Mark(scene::Stage::WorldSceneEnter, "WorldSceneEnter marker"))
      return;
    g.tracker.mark(uint32_t(scene::Stage::WorldSceneEnter), g.draws);
    ++g.status.worldScenes;
  } else {
    if (!g.tracker.active || !Mark(scene::Stage::WorldSceneExit, "WorldSceneExit marker"))
      return;
    g.tracker.mark(uint32_t(scene::Stage::WorldSceneExit), g.draws);
  }
}

// Game.dll's CGxDeviceD3d::SetMaterial (0x6F0EF1BF) always copies Diffuse into
// Ambient (mat.Ambient = mat.Diffuse), carrying Fog-of-War dimming, unexplored
// Black Mask (0,0,0), object vertex-color tints, and unshaded/emissive-only
// layers (Infernal glow: D=0 A=0 E=(0.8,1,0)). Forcing white material ambient
// over those draws lit up unexplored doodads/destructibles in pitch-black mask,
// over-brightened fog-of-war destructibles, and washed out emissive glows.
// Meanwhile, non-terrain D3DMCS_COLOR1 draws (water, ground splats/decals) carry
// Fog-of-War / Black Mask in vertex colour in_Color0; the front end's
// ambientOverride forces D3DRS_AMBIENTMATERIALSOURCE = D3DMCS_MATERIAL, which
// would discard in_Color0 on water and light it up inside Black Mask.
// Only terrain Stage 1 (which gets Fog-of-War from Texture Stage 1 and leaves
// material Ambient stale) needs the forced white material ambient override.
// The front end applies the world lighting when it sends the draw, after this
// callback, so the change holds for exactly the draws that need it.
static void EmissiveOnlyDraw(IDirect3DDevice9* device) {
  if (!g.worldLightingHeld)
    return;
  DWORD ambientSource = D3DMCS_MATERIAL;
  const bool haveSource = SUCCEEDED(device->GetRenderState(D3DRS_AMBIENTMATERIALSOURCE, &ambientSource));
  const bool isWater = War3RenderState::IsWaterRendering();
  const bool isTerrainBase = !isWater &&
                             War3RenderState::GetStageCategory() == War3RenderState::StageCategory::Terrain &&
                             War3RenderState::GetStage() == 1;

  uint32_t wantOverride = g.worldLighting.ambientOverride;
  uint32_t want = g.worldLighting.materialAmbientWhite;

  if (isWater || (haveSource && ambientSource == D3DMCS_COLOR1 && !isTerrainBase)) {
    // Preserve D3DMCS_COLOR1 so vertex-color Fog-of-War / Black Mask modulates
    // both global ambient and Light0 ambient (water, vertex-colored decals).
    wantOverride = 0u;
    want = 0u;
    if (++g.colorSourceDraws == 1u)
      Logger::info(str::format("WarVK proxy mode: vertex-color draw keeps D3DMCS_COLOR1 (water=",
                               isWater ? 1 : 0, " stage=", War3RenderState::GetStage(), ")"));
  } else if (!isTerrainBase && g.worldLighting.materialAmbientWhite) {
    D3DMATERIAL9 m = {};
    if (SUCCEEDED(device->GetMaterial(&m))) {
      const bool zeroDiffuse = m.Diffuse.r == 0.0f && m.Diffuse.g == 0.0f && m.Diffuse.b == 0.0f;
      const bool emissiveOnly = zeroDiffuse &&
                                (m.Emissive.r > 0.0f || m.Emissive.g > 0.0f || m.Emissive.b > 0.0f);
      const bool hasGameAmbient = m.Ambient.r > 0.0f || m.Ambient.g > 0.0f || m.Ambient.b > 0.0f;
      // Keep the game's material Ambient whenever it is already populated
      // (active vision 1,1,1; explored Fog of War ~0.35; unit/doodad tint) or
      // when Diffuse is black (unexplored Black Mask, fog-hidden destructible,
      // or emissive-only glow layer).
      want = (zeroDiffuse || hasGameAmbient) ? 0u : 1u;
      if (emissiveOnly && ++g.emissiveOnlyDraws == 1u)
        Logger::info(str::format("WarVK proxy mode: emissive-only draw keeps the game's ambient (emissive=",
                                 m.Emissive.r, ",", m.Emissive.g, ",", m.Emissive.b, ")"));
      else if (zeroDiffuse && !emissiveOnly && ++g.fogTintedDraws == 1u)
        Logger::info("WarVK proxy mode: black-mask/fogged draw keeps the game's zero material ambient");
    }
  }

  if (want == g.ambientWhiteHeld && wantOverride == g.ambientOverrideHeld)
    return;
  WarVKD3D9WorldLighting lighting = g.worldLighting;
  lighting.materialAmbientWhite = want;
  lighting.ambientOverride = wantOverride;
  if (SetWorldLighting(&lighting) == S_OK) {
    g.ambientWhiteHeld = want;
    g.ambientOverrideHeld = wantOverride;
  }
}

// The game draws a few overlays inside the world scene in camera space (the
// view transform is the identity): the cinematic filter is one, a lit quad
// whose ambient and diffuse come from its vertex colour, so a fade to black
// is a black quad. The in-process device leaves the game's own ambient source
// to a draw that set it; the front end forces the material's (white) on every
// world draw, and the black quad took our ambient: the fades of campaign
// cinematics and of a map's start went to grey or blue, the world's shadows
// drawn over them. Such an overlay gets the game's own lighting.
static bool CameraSpaceDraw(IDirect3DDevice9* device) {
  D3DMATRIX view = {};
  if (FAILED(device->GetTransform(D3DTS_VIEW, &view)))
    return false;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c)
      if (view.m[r][c] != (r == c ? 1.0f : 0.0f))
        return false;
  return true;
}

// The world lighting for one world draw, before the front end sends it: none
// for a camera-space overlay, else the world lighting with Fog-of-War /
// Black-Mask / emissive-only / D3DMCS_COLOR1 ambient preservation.
static void WorldDrawLighting(IDirect3DDevice9* device) {
  if (!g.worldLightingHeld)
    return;
  const bool overlay = CameraSpaceDraw(device);
  if (overlay) {
    if (!g.overlayHeld && SetWorldLighting(nullptr) == S_OK) {
      g.overlayHeld = true;
      if (++g.overlayDraws == 1u)
        Logger::info("WarVK proxy mode: a camera-space overlay in the world keeps the game's lighting");
    }
    return;
  }
  if (g.overlayHeld) {
    WarVKD3D9WorldLighting lighting = g.worldLighting;
    lighting.materialAmbientWhite = g.ambientWhiteHeld;
    lighting.ambientOverride = g.ambientOverrideHeld;
    if (SetWorldLighting(&lighting) != S_OK)
      return;
    g.overlayHeld = false;
  }
  EmissiveOnlyDraw(device);
}

void FrameDraw(IDirect3DDevice9* device, const WarVKD3D9DrawFacts& facts) {
  if (!g.connected)
    return;
  if (g.inWorldScene)
    WorldDrawLighting(device);
  // The camera is sampled even when nothing is sent: the control plane's
  // "in game" signal comes from it.
  if (g.inWorldScene) {
    SampleCamera(device);
    // A world draw may cast a shadow: its record goes with it.
    if (Sending())
      Sent(CastersDraw(device, facts), "ShadowCaster record");
  } else if (Sending() && g.tracker.canInsertBeforeUi())
    InsertBeforeUi(false);
  ++g.draws;
}

// One line of the diagnostics block per game frame: what happened on this side
// of the proxy, for the frame evidence (war3_proxy_diag_block.h).
void PublishDiagnosticFrame() {
  static FrameStatus before;
  static CasterStatus castersBefore;
  static LARGE_INTEGER lastPresent = {};
  LARGE_INTEGER now = {}, frequency = {};
  ::QueryPerformanceCounter(&now);
  ::QueryPerformanceFrequency(&frequency);
  const CasterStatus& casters = CastersStatus();
  const FrameStatus& status = g.status;
  const bool inserted = status.insertions != before.insertions;
  warvk::diag::GameFrame out = {};
  out.qpc = uint64_t(now.QuadPart);
  out.frame = uint32_t(g.frame);
  if (lastPresent.QuadPart != 0 && frequency.QuadPart > 0)
    out.intervalUs = uint32_t((now.QuadPart - lastPresent.QuadPart) * 1000000 / frequency.QuadPart);
  out.flags = (status.worldScenes != before.worldScenes ? 1u : 0u) | (inserted ? 2u : 0u) |
      (status.insertionsAtPresent != before.insertionsAtPresent ? 4u : 0u) |
      (status.cameraFallbacks != before.cameraFallbacks ? 8u : 0u) |
      (status.framesWithoutCamera != before.framesWithoutCamera ? 16u : 0u) |
      (status.sendingStopped ? 32u : 0u);
  out.cameraTier = g.cameraTier;
  if (inserted) {
    out.castersConsidered = casters.lastFrame.considered;
    out.castersSent = casters.lastFrame.sent;
    out.castersOmitted = g.lastCasters.omitted;
    out.casterTerrain = casters.lastFrame.terrain;
    out.casterDoodads = casters.lastFrame.doodads;
    out.casterUnits = casters.lastFrame.units;
    out.casterWorldObjects = casters.lastFrame.worldObjects;
  }
  const auto counts = inserted && (status.lastRecordMask & (1u << scene::ShadowFrameSchema))
      ? casters.lastFrame : CastersCurrentCounts();
  out.demandCasters = counts.demandCasters;
  out.demandBytes = counts.demandBytes;
  out.admittedBytes = counts.admittedBytes;
  out.casterBytes = uint32_t(casters.dynamicBytes - castersBefore.dynamicBytes);
  out.timeOfDay = War3RenderState::GetGameTime();
  DiagGameFrame(out);
  lastPresent = now;
  before = status;
  castersBefore = casters;
}

void FramePresent(IDirect3DDevice9*) {
  if (Sending() && g.tracker.canInsertBeforeUi())
    InsertBeforeUi(true);
  if (g.connected)
    PublishDiagnosticFrame();

  // The frame ends here for both ends: the host clears its packet at Present.
  g.tracker.frameEnd();
  g.haveCamera = false;
  g.cameraTier = 0u;
  g.cameraArea = 0u;
  ++g.frame;
  ++g.status.frames;
  if (g.connected) {
    ResolveSettings();
    // The host cleared its packet and the casters with it.
    CastersBeginFrame(g.settings);
  } else {
    CastersEndFrame();
  }
  // Every frame: the control plane's proxyMode section reads this copy.
  Publish();
  if (g.connected)
    ExportStatusPeriodically();
  // A line now and then: a session that does not end in order leaves no
  // closing account.
  if (g.connected && (g.status.frames % 900u) == 0u)
    LogCasters();
}

uint64_t FrameSessionReset() {
  g.haveCamera = g.haveLastGood = false;
  g.cameraTier = 0u;
  g.cameraArea = 0u;
  ++g.status.sessionGeneration;
  // The host drops the casters of the old map with its scene.
  CastersEndFrame();
  // As the host's tracker: the generation ascends, nothing else changes.
  g.tracker.generation = g.status.sessionGeneration;
  Publish();
  return g.status.sessionGeneration;
}

FrameStatus QueryFrameStatus() noexcept {
  std::lock_guard<std::mutex> lock(g_statusMutex);
  return g_published;
}

nlohmann::json StatusJson() {
  if (!Requested())
    return nullptr;
  const Status session = QueryStatus();
  const FrameStatus frame = QueryFrameStatus();
  const CasterStatus& casters = frame.casters;
  const CasterFrameCounts& last = casters.lastFrame;
  nlohmann::json skipped = nlohmann::json::array();
  for (uint64_t count : casters.skipped)
    skipped.push_back(count);
  return nlohmann::json{
      {"schema", "warvk-proxy-mode-status/v1"},
      {"mode", "proxy"},
      {"frontLoaded", session.frontLoaded},
      {"deviceConnected", session.deviceConnected},
      // The host session ended (war3_proxy_host_lost.h): the device is gone.
      {"deviceLost", session.terminal},
      {"terminalKind", std::string(session.terminalKind)},
      {"terminalDetail", std::string(session.terminalDetail)},
      {"factories", session.factories},
      {"devices", session.devices},
      {"draws", session.draws},
      {"presents", session.presents},
      {"width", session.width},
      {"height", session.height},
      {"frame",
       {{"frames", frame.frames},
        {"worldScenes", frame.worldScenes},
        {"insertions", frame.insertions},
        {"insertionsAtPresent", frame.insertionsAtPresent},
        {"framesWithoutCamera", frame.framesWithoutCamera},
        {"cameraFallbacks", frame.cameraFallbacks},
        {"records", frame.records},
        {"refused", frame.refused},
        {"sessionGeneration", frame.sessionGeneration},
        {"lastRecordMask", frame.lastRecordMask},
        {"sendingStopped", frame.sendingStopped}}},
      {"casters",
       {{"considered", casters.considered},
        {"sent", casters.sent},
        {"alpha", casters.alpha},
        {"terrain", casters.terrain},
        {"doodads", casters.doodads},
        {"worldObjects", casters.worldObjects},
        {"units", casters.units},
        {"dynamicBytes", casters.dynamicBytes},
        {"omittedFrames", casters.omittedFrames},
        {"skipped", skipped},
        {"withHandle", casters.withHandle},
        {"unitsWithHandle", casters.unitsWithHandle},
        {"outlineMatched", casters.outlineMatched},
        {"lastFrame",
         {{"considered", last.considered},
          {"sent", last.sent},
          {"terrain", last.terrain},
          {"doodads", last.doodads},
          {"worldObjects", last.worldObjects},
          {"units", last.units},
          {"withHandle", last.withHandle},
          {"unitsWithHandle", last.unitsWithHandle},
          {"outlineMatched", last.outlineMatched}}}}},
      {"outline",
       {{"handleCount", War3RenderState::GetOutlineHandleCount()}}},
  };
}

} // namespace dxvk::war3::proxy
