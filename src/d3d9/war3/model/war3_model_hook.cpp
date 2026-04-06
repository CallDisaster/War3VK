// war3_model_hook.cpp - War3 runtime-model / pose 被动探针

#include "war3_model_hook.h"

#include "war3_model_registry.h"

#include "../../d3d9_war3_debug.h"
#include "../core/war3_game_structs.h"
#include "../core/war3_internal_test_config.h"
#include "../core/war3_memory.h"
#include "../game/war3_unit.h"
#include "../hooks/war3_hook_install_util.h"
#include "../render/war3_shadow_object_registry.h"
#include "../render/war3_shadow_runtime_bridge.h"
#include "../../util/util_env.h"

#include <emmintrin.h>

#include <atomic>
#include <cstring>
#include <string>

namespace dxvk {
namespace war3 {
namespace model {

namespace {
using CreateSpriteRuntimeFn = void *(__thiscall *)(void *thisPtr);
using SpriteFrameUpdateFn =
    int(__thiscall *)(int thisPtr, float dt, int a3, unsigned int a4, int a5);
using SpriteFrameLiteUpdateFn = int(__thiscall *)(int thisPtr, float dt);
using RuntimePoseUpdateFn =
    int(__fastcall *)(int runtimeModel, const __m128i *poseMatrix, float scale,
                      int a4, int a5);
using RuntimeMatrixRangeCopyFn = int(__fastcall *)(int runtimeModel, int a2,
                                                   int a3);
using RuntimeMatrixFlushFn = int(__thiscall *)(int runtimeModel);

struct HookConfig {
  bool enabled = true;
  bool logEnabled = false;
  bool poseEnabled = true;
};

constexpr uintptr_t kCreateSpriteRuntimeRva = 0x185250;
constexpr uintptr_t kSpriteFrameUpdateRva = 0x182300;
constexpr uintptr_t kSpriteFrameLiteUpdateRva = 0x1826C0;
constexpr uintptr_t kRuntimePoseUpdateRva = 0x12F0A0;
constexpr uintptr_t kRuntimeMatrixRangeCopyRva = 0x12FDC0;
constexpr uintptr_t kRuntimeMatrixFlushRva = 0x12FF50;
constexpr size_t kSourceModelResourceOffset = 0x20; // this[8]
constexpr size_t kSourceFlagsOffset = 0x28;         // this[10]
constexpr size_t kRuntimeMatrixCountOffset = 0x5C;
constexpr size_t kRuntimeMatrixArrayOffset = 0x60;

std::atomic<bool> g_active{false};
std::atomic<uint32_t> g_bindLogCount{0};
std::atomic<uint32_t> g_poseLogCount{0};
std::atomic<uint32_t> g_spriteFrameLogCount{0};

CreateSpriteRuntimeFn g_trampolineCreateSpriteRuntime = nullptr;
SpriteFrameUpdateFn g_trampolineSpriteFrameUpdate = nullptr;
SpriteFrameLiteUpdateFn g_trampolineSpriteFrameLiteUpdate = nullptr;
RuntimePoseUpdateFn g_trampolineRuntimePoseUpdate = nullptr;
RuntimeMatrixRangeCopyFn g_trampolineRuntimeMatrixRangeCopy = nullptr;
RuntimeMatrixFlushFn g_trampolineRuntimeMatrixFlush = nullptr;
HookConfig g_config = {};

bool GetEnvBoolCached(const char *name, bool defaultValue) {
  const std::string value = env::getEnvVar(name);
  if (value.empty())
    return defaultValue;
  return value != "0";
}

uint64_t HashMatrixPalette(const std::vector<Matrix4>& matrices) {
  uint64_t hash = 1469598103934665603ull;
  const auto* bytes = reinterpret_cast<const uint8_t*>(matrices.data());
  const size_t size = matrices.size() * sizeof(Matrix4);
  for (size_t i = 0; i < size; ++i) {
    hash ^= uint64_t(bytes[i]);
    hash *= 1099511628211ull;
  }
  return hash;
}

void MaybeLogBinding(void *spritePtr, void *runtimeModelPtr,
                     void *modelResourcePtr, uint64_t modelKey) {
  if (!g_config.logEnabled)
    return;

  const uint32_t count = g_bindLogCount.fetch_add(1, std::memory_order_relaxed);
  if (count < 32 || (count % 512u) == 0u) {
    war3dbg::Print(
        "DXVK_Model: bind sprite=%p runtime=%p model=%p key=0x%llX\n",
        spritePtr, runtimeModelPtr, modelResourcePtr,
        static_cast<unsigned long long>(modelKey));
  }
}

void MaybeLogPose(void *runtimeModelPtr, void *sceneNode, void *unitPtr,
                  float scale) {
  if (!g_config.logEnabled)
    return;

  const uint32_t count = g_poseLogCount.fetch_add(1, std::memory_order_relaxed);
  if (count < 16 || (count % 2048u) == 0u) {
    war3dbg::Print("DXVK_Model: pose runtime=%p scene=%p unit=%p scale=%.3f\n",
                   runtimeModelPtr, sceneNode, unitPtr, double(scale));
  }
}

void MaybeLogSpriteFrame(void* spritePtr, void* runtimeModelPtr, void* sceneNode,
                         void* unitPtr, float dt, uint32_t matrixCount) {
  if (!g_config.logEnabled)
    return;

  const uint32_t count =
      g_spriteFrameLogCount.fetch_add(1, std::memory_order_relaxed);
  if (count < 16 || (count % 2048u) == 0u) {
    war3dbg::Print(
        "DXVK_Model: spriteFrame sprite=%p runtime=%p scene=%p unit=%p dt=%.4f matrices=%u\n",
        spritePtr, runtimeModelPtr, sceneNode, unitPtr, double(dt),
        unsigned(matrixCount));
  }
}

Matrix4 DecodeRuntimePoseMatrix(const __m128i *poseMatrix) {
  if (poseMatrix == nullptr)
    return Matrix4();

  float pose3x4[12] = {};
  std::memcpy(pose3x4, poseMatrix, sizeof(pose3x4));
  return Matrix4(Vector4(pose3x4[0], pose3x4[1], pose3x4[2], 0.0f),
                 Vector4(pose3x4[3], pose3x4[4], pose3x4[5], 0.0f),
                 Vector4(pose3x4[6], pose3x4[7], pose3x4[8], 0.0f),
                 Vector4(pose3x4[9], pose3x4[10], pose3x4[11], 1.0f));
}

Matrix4 DecodeRuntimePoseMatrix48(const uint8_t* poseBytes) {
  if (poseBytes == nullptr)
    return Matrix4();

  float pose3x4[12] = {};
  std::memcpy(pose3x4, poseBytes, sizeof(pose3x4));
  return Matrix4(Vector4(pose3x4[0], pose3x4[1], pose3x4[2], 0.0f),
                 Vector4(pose3x4[3], pose3x4[4], pose3x4[5], 0.0f),
                 Vector4(pose3x4[6], pose3x4[7], pose3x4[8], 0.0f),
                 Vector4(pose3x4[9], pose3x4[10], pose3x4[11], 1.0f));
}

void *TryReadPtrFast(const void *base, size_t offset) {
  void *value = nullptr;
  if (!base)
    return nullptr;
  if (!SafeReadPtrFast(base, offset, value))
    return nullptr;
  return value;
}

uint32_t TryReadU32Fast(const void *base, size_t offset) {
  uint32_t value = 0;
  if (!base)
    return 0;
  SafeReadU32Fast(base, offset, value);
  return value;
}

float TryReadF32Fast(const void* base, size_t offset) {
  float value = 0.0f;
  if (!base)
    return 0.0f;
  SafeReadFast(base, offset, value);
  return value;
}

bool TryReadSpriteFrameTransform(void* spritePtr, Matrix4& out,
                                 float& outScale, float& outSequenceTime) {
  if (!spritePtr)
    return false;

  float raw[12] = {};
  if (!IsReadableRange(reinterpret_cast<const uint8_t*>(spritePtr) +
                           dxvk::war3::CSpriteUberOffsets::WorldMatrix3x4,
                       sizeof(raw))) {
    return false;
  }

  std::memcpy(
      raw,
      reinterpret_cast<const uint8_t*>(spritePtr) +
          dxvk::war3::CSpriteUberOffsets::WorldMatrix3x4,
      sizeof(raw));
  out = Matrix4(Vector4(raw[0], raw[1], raw[2], 0.0f),
                Vector4(raw[3], raw[4], raw[5], 0.0f),
                Vector4(raw[6], raw[7], raw[8], 0.0f),
                Vector4(raw[9], raw[10], raw[11], 1.0f));

  outScale = TryReadF32Fast(spritePtr, dxvk::war3::CSpriteUberOffsets::UniformScale);
  if (outScale == 0.0f)
    outScale = 1.0f;

  const uint32_t overrideEnabled = TryReadU32Fast(
      spritePtr, dxvk::war3::CSpriteUberOffsets::AnimationTimeOverrideEnabled);
  outSequenceTime =
      overrideEnabled != 0
          ? TryReadF32Fast(
                spritePtr,
                dxvk::war3::CSpriteUberOffsets::AnimationTimeOverrideValue)
          : 0.0f;
  return true;
}

bool TryReadRuntimeMatrixPalette(int runtimeModel, std::vector<Matrix4>& out) {
  if (runtimeModel == 0)
    return false;

  uint32_t matrixCount = 0;
  void* matrixBase = nullptr;
  if (!SafeReadU32Fast(reinterpret_cast<void*>(runtimeModel),
                       kRuntimeMatrixCountOffset, matrixCount) ||
      !SafeReadPtrFast(reinterpret_cast<void*>(runtimeModel),
                       kRuntimeMatrixArrayOffset, matrixBase) ||
      matrixBase == nullptr || matrixCount == 0) {
    return false;
  }

  matrixCount = std::min<uint32_t>(matrixCount, 256u);
  const size_t bytes = size_t(matrixCount) * 48u;
  if (!IsReadableRange(matrixBase, bytes))
    return false;

  out.resize(matrixCount);
  auto* raw = reinterpret_cast<const uint8_t*>(matrixBase);
  for (uint32_t i = 0; i < matrixCount; ++i)
    out[i] = DecodeRuntimePoseMatrix48(raw + size_t(i) * 48u);
  return true;
}

void RecordRuntimeModelBinding(void *sourcePtr, void *spritePtr) {
  if (!sourcePtr || !spritePtr)
    return;

  void *modelResourcePtr = TryReadPtrFast(sourcePtr, kSourceModelResourceOffset);
  void *runtimeModelPtr =
      TryReadPtrFast(spritePtr, dxvk::war3::CSpriteOffsets::Model);
  const uint32_t sourceFlags = TryReadU32Fast(sourcePtr, kSourceFlagsOffset);

  if (!runtimeModelPtr)
    return;

  auto &modelRegistry = ModelRegistry::instance();
  modelRegistry.recordRuntimeModelBinding(spritePtr, runtimeModelPtr,
                                          modelResourcePtr, 0u, sourceFlags);

  ModelResourceRecord resourceRecord = {};
  if (modelRegistry.findByRuntimeModel(runtimeModelPtr, resourceRecord)) {
    render::NoteShadowRuntimeModelBinding(
        spritePtr, runtimeModelPtr, resourceRecord.modelResourcePtr,
        resourceRecord.modelPath, resourceRecord.modelType,
        resourceRecord.flags, resourceRecord.modelKey);
    MaybeLogBinding(spritePtr, runtimeModelPtr, resourceRecord.modelResourcePtr,
                    resourceRecord.modelKey);
    return;
  }

  render::NoteShadowRuntimeModelBinding(spritePtr, runtimeModelPtr,
                                        modelResourcePtr, std::string(), 0u,
                                        sourceFlags, 0u);
  MaybeLogBinding(spritePtr, runtimeModelPtr, modelResourcePtr, 0u);
}

void RecordRuntimePose(int runtimeModel, const __m128i *poseMatrix, float scale) {
  if (runtimeModel == 0)
    return;

  const Matrix4 worldTransform = DecodeRuntimePoseMatrix(poseMatrix);
  ModelInstanceRecord instanceRecord = {};
  if (!ModelInstanceRegistry::instance().findByRuntimeModel(
          reinterpret_cast<void *>(runtimeModel), instanceRecord)) {
    render::NoteShadowRuntimePose(reinterpret_cast<void *>(runtimeModel),
                                  nullptr, nullptr, 0u, 0.0f, scale, 0.0f,
                                  0.0f, 0.0f, 0.0f, true, &worldTransform);
    MaybeLogPose(reinterpret_cast<void *>(runtimeModel), nullptr, nullptr,
                 scale);
    return;
  }

  float flyHeight = 0.0f;
  if (instanceRecord.unitPtr) {
    game::UnitWrapper unit(instanceRecord.unitPtr);
    if (unit.IsValid())
      flyHeight = unit.GetFlyHeight();
  }

  render::NoteShadowRuntimePose(reinterpret_cast<void *>(runtimeModel),
                                instanceRecord.sceneNode,
                                instanceRecord.unitPtr, 0u, 0.0f, scale,
                                0.0f, 0.0f, 0.0f, flyHeight, true,
                                &worldTransform);
  MaybeLogPose(reinterpret_cast<void *>(runtimeModel), instanceRecord.sceneNode,
               instanceRecord.unitPtr, scale);
}

void RecordRuntimeMatrixPalette(int runtimeModel) {
  if (runtimeModel == 0)
    return;

  const void* runtimeModelPtr = reinterpret_cast<void*>(runtimeModel);
  const uint64_t poseFrame = PoseRegistry::instance().frameNumber();
  PoseRecord existingRecord = {};
  if (PoseRegistry::instance().findByRuntimeModel(
          const_cast<void*>(runtimeModelPtr), existingRecord) &&
      existingRecord.lastMatrixPaletteFrame == poseFrame &&
      existingRecord.matrixCount != 0 &&
      !existingRecord.matrixPalette.empty()) {
    return;
  }

  std::vector<Matrix4> matrices;
  if (!TryReadRuntimeMatrixPalette(runtimeModel, matrices))
    return;

  ModelInstanceRecord instanceRecord = {};
  ModelInstanceRegistry::instance().findByRuntimeModel(
      const_cast<void*>(runtimeModelPtr), instanceRecord);

  const uint64_t matrixHash = matrices.empty() ? 0ull : HashMatrixPalette(matrices);
  PoseRegistry::instance().recordMatrixPalette(
      const_cast<void*>(runtimeModelPtr), instanceRecord.sceneNode,
      instanceRecord.unitPtr, matrices.data(), uint32_t(matrices.size()));
  render::NoteShadowRuntimePose(
      const_cast<void*>(runtimeModelPtr), instanceRecord.sceneNode,
      instanceRecord.unitPtr, 0u, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, false,
      nullptr, uint32_t(matrices.size()), matrixHash);
}

void RecordSpriteFramePoseFromSprite(int spritePtr, float dt) {
  if (spritePtr == 0)
    return;

  void* spriteRaw = reinterpret_cast<void*>(spritePtr);
  void* runtimeModelPtr =
      TryReadPtrFast(spriteRaw, dxvk::war3::CSpriteOffsets::Model);
  if (!runtimeModelPtr)
    return;

  Matrix4 worldTransform;
  float scale = 1.0f;
  float sequenceTime = 0.0f;
  const bool hasWorldTransform =
      TryReadSpriteFrameTransform(spriteRaw, worldTransform, scale, sequenceTime);

  ModelInstanceRecord instanceRecord = {};
  if (!ModelInstanceRegistry::instance().findByRuntimeModel(runtimeModelPtr,
                                                            instanceRecord)) {
    ModelInstanceRegistry::instance().findBySpritePtr(spriteRaw, instanceRecord);
  }

  float flyHeight = 0.0f;
  if (instanceRecord.unitPtr) {
    game::UnitWrapper unit(instanceRecord.unitPtr);
    if (unit.IsValid())
      flyHeight = unit.GetFlyHeight();
  }

  render::NoteShadowRuntimeSpriteFramePose(
      runtimeModelPtr, spriteRaw, instanceRecord.sceneNode, instanceRecord.unitPtr,
      dt, 0u, sequenceTime, scale, 0.0f, 0.0f, 0.0f, flyHeight,
      hasWorldTransform, hasWorldTransform ? &worldTransform : nullptr,
      0u, 0ull);

  MaybeLogSpriteFrame(spriteRaw, runtimeModelPtr, instanceRecord.sceneNode,
                      instanceRecord.unitPtr, dt, 0u);
}

void *__fastcall Hook_CreateSpriteRuntime(void *thisPtr, void *edx) {
  if (!g_trampolineCreateSpriteRuntime)
    return nullptr;

  void *spritePtr = g_trampolineCreateSpriteRuntime(thisPtr);
  RecordRuntimeModelBinding(thisPtr, spritePtr);
  return spritePtr;
}

int __fastcall Hook_RuntimePoseUpdate(int runtimeModel,
                                      const __m128i *poseMatrix, float scale,
                                      int a4, int a5) {
  if (!g_trampolineRuntimePoseUpdate)
    return 0;

  const int result =
      g_trampolineRuntimePoseUpdate(runtimeModel, poseMatrix, scale, a4, a5);
  RecordRuntimePose(runtimeModel, poseMatrix, scale);
  return result;
}

int __fastcall Hook_SpriteFrameUpdate(int thisPtr, void* edx, float dt, int a3,
                                      unsigned int a4, int a5) {
  if (!g_trampolineSpriteFrameUpdate)
    return 0;

  const int result =
      g_trampolineSpriteFrameUpdate(thisPtr, dt, a3, a4, a5);
  RecordSpriteFramePoseFromSprite(thisPtr, dt);
  return result;
}

int __fastcall Hook_SpriteFrameLiteUpdate(int thisPtr, void* edx, float dt) {
  if (!g_trampolineSpriteFrameLiteUpdate)
    return 0;

  const int result = g_trampolineSpriteFrameLiteUpdate(thisPtr, dt);
  RecordSpriteFramePoseFromSprite(thisPtr, dt);
  return result;
}

int __fastcall Hook_RuntimeMatrixRangeCopy(int runtimeModel, int a2, int a3) {
  if (!g_trampolineRuntimeMatrixRangeCopy)
    return 0;

  const int result = g_trampolineRuntimeMatrixRangeCopy(runtimeModel, a2, a3);
  RecordRuntimeMatrixPalette(runtimeModel);
  return result;
}

int __fastcall Hook_RuntimeMatrixFlush(int runtimeModel, void* edx) {
  if (!g_trampolineRuntimeMatrixFlush)
    return 0;

  const int result = g_trampolineRuntimeMatrixFlush(runtimeModel);
  RecordRuntimeMatrixPalette(runtimeModel);
  return result;
}

bool InstallCreateSpriteRuntimeHook(uintptr_t gameBase) {
  const uintptr_t target = gameBase + kCreateSpriteRuntimeRva;
  if (!IsExecutableRange(reinterpret_cast<const void *>(target), 16)) {
    war3dbg::Print(
        "DXVK_Model: runtime sprite ctor 不可执行，跳过 Hook (addr=%p)\n",
        reinterpret_cast<void *>(target));
    return false;
  }

  return hooks::InstallMinHook(
      reinterpret_cast<LPVOID>(target),
      reinterpret_cast<LPVOID>(&Hook_CreateSpriteRuntime),
      reinterpret_cast<LPVOID *>(&g_trampolineCreateSpriteRuntime), "Model",
      "CreateSpriteRuntime", true, g_config.logEnabled);
}

bool InstallSpriteFrameUpdateHook(uintptr_t gameBase) {
  const uintptr_t target = gameBase + kSpriteFrameUpdateRva;
  if (!IsExecutableRange(reinterpret_cast<const void *>(target), 16)) {
    war3dbg::Print(
        "DXVK_Model: sprite frame update 不可执行，跳过 Hook (addr=%p)\n",
        reinterpret_cast<void *>(target));
    return false;
  }

  return hooks::InstallMinHook(
      reinterpret_cast<LPVOID>(target),
      reinterpret_cast<LPVOID>(&Hook_SpriteFrameUpdate),
      reinterpret_cast<LPVOID *>(&g_trampolineSpriteFrameUpdate), "Model",
      "SpriteFrameUpdate", true, g_config.logEnabled);
}

bool InstallSpriteFrameLiteUpdateHook(uintptr_t gameBase) {
  const uintptr_t target = gameBase + kSpriteFrameLiteUpdateRva;
  if (!IsExecutableRange(reinterpret_cast<const void *>(target), 16)) {
    war3dbg::Print(
        "DXVK_Model: sprite frame lite update 不可执行，跳过 Hook (addr=%p)\n",
        reinterpret_cast<void *>(target));
    return false;
  }

  return hooks::InstallMinHook(
      reinterpret_cast<LPVOID>(target),
      reinterpret_cast<LPVOID>(&Hook_SpriteFrameLiteUpdate),
      reinterpret_cast<LPVOID *>(&g_trampolineSpriteFrameLiteUpdate), "Model",
      "SpriteFrameLiteUpdate", true, g_config.logEnabled);
}

bool InstallRuntimePoseHook(uintptr_t gameBase) {
  const uintptr_t target = gameBase + kRuntimePoseUpdateRva;
  if (!IsExecutableRange(reinterpret_cast<const void *>(target), 16)) {
    war3dbg::Print(
        "DXVK_Model: runtime pose update 不可执行，跳过 Hook (addr=%p)\n",
        reinterpret_cast<void *>(target));
    return false;
  }

  return hooks::InstallMinHook(
      reinterpret_cast<LPVOID>(target),
      reinterpret_cast<LPVOID>(&Hook_RuntimePoseUpdate),
      reinterpret_cast<LPVOID *>(&g_trampolineRuntimePoseUpdate), "Model",
      "RuntimePoseUpdate", true, g_config.logEnabled);
}

bool InstallRuntimeMatrixRangeCopyHook(uintptr_t gameBase) {
  const uintptr_t target = gameBase + kRuntimeMatrixRangeCopyRva;
  if (!IsExecutableRange(reinterpret_cast<const void*>(target), 16)) {
    war3dbg::Print(
        "DXVK_Model: runtime matrix range copy 不可执行，跳过 Hook (addr=%p)\n",
        reinterpret_cast<void*>(target));
    return false;
  }

  return hooks::InstallMinHook(
      reinterpret_cast<LPVOID>(target),
      reinterpret_cast<LPVOID>(&Hook_RuntimeMatrixRangeCopy),
      reinterpret_cast<LPVOID*>(&g_trampolineRuntimeMatrixRangeCopy), "Model",
      "RuntimeMatrixRangeCopy", true, g_config.logEnabled);
}

bool InstallRuntimeMatrixFlushHook(uintptr_t gameBase) {
  const uintptr_t target = gameBase + kRuntimeMatrixFlushRva;
  if (!IsExecutableRange(reinterpret_cast<const void*>(target), 16)) {
    war3dbg::Print(
        "DXVK_Model: runtime matrix flush 不可执行，跳过 Hook (addr=%p)\n",
        reinterpret_cast<void*>(target));
    return false;
  }

  return hooks::InstallMinHook(
      reinterpret_cast<LPVOID>(target),
      reinterpret_cast<LPVOID>(&Hook_RuntimeMatrixFlush),
      reinterpret_cast<LPVOID*>(&g_trampolineRuntimeMatrixFlush), "Model",
      "RuntimeMatrixFlush", true, g_config.logEnabled);
}
} // namespace

void Init(uintptr_t gameBase) {
  if (g_active.load(std::memory_order_relaxed))
    return;

  g_config.enabled = GetEnvBoolCached(
      "DXVK_WAR3_MODEL_HOOK",
      dxvk::war3::internal::kShadowRuntimeModelHookEnabled);
  g_config.logEnabled = GetEnvBoolCached("DXVK_WAR3_MODEL_LOG", false);
  // runtime-model / pose 链目前已经具备研究/验证价值，但在“上游结构直传 +
  // 动态单位姿态主路径”真正接通前，默认常驻开启仍会带来明显 CPU 回退，
  // 且动态单位阴影正确性还不够稳定。生产默认保持关闭，按需用环境变量显式
  // 开启进行专项验证：
  //   DXVK_WAR3_MODEL_POSE_HOOK=1
  g_config.poseEnabled = GetEnvBoolCached("DXVK_WAR3_MODEL_POSE_HOOK", false);

  war3dbg::Print("DXVK_Model: init enabled=%d pose=%d log=%d\n",
                 g_config.enabled ? 1 : 0, g_config.poseEnabled ? 1 : 0,
                 g_config.logEnabled ? 1 : 0);

  if (!g_config.enabled || !gameBase)
    return;

  bool installed = InstallCreateSpriteRuntimeHook(gameBase);
  if (g_config.poseEnabled)
    installed = InstallSpriteFrameUpdateHook(gameBase) || installed;
  if (g_config.poseEnabled)
    installed = InstallSpriteFrameLiteUpdateHook(gameBase) || installed;
  if (g_config.poseEnabled)
    installed = InstallRuntimePoseHook(gameBase) || installed;
  if (g_config.poseEnabled)
    installed = InstallRuntimeMatrixRangeCopyHook(gameBase) || installed;
  if (g_config.poseEnabled)
    installed = InstallRuntimeMatrixFlushHook(gameBase) || installed;

  g_active.store(installed, std::memory_order_relaxed);
}

void Shutdown() {
  g_active.store(false, std::memory_order_relaxed);
}

bool IsActive() { return g_active.load(std::memory_order_relaxed); }

bool IsPoseHookEnabled() { return g_config.poseEnabled; }

} // namespace model
} // namespace war3
} // namespace dxvk
