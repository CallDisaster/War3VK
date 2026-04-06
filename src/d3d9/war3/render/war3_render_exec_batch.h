// war3_render_exec_batch.h - ExecBatch processing helpers

#pragma once

#include "war3_render_state.h"

namespace dxvk {
namespace war3 {
namespace render {

struct ExecBatchContext {
  int prevStage = -1;
  War3BatchTag prevTag = War3BatchTag::Unknown;
  bool stageOverridden = false;
  War3TlsShadowSemanticState prevShadowSemantic = {};
};

class ExecBatchProcessor {
public:
  static ExecBatchContext Begin(void *element, War3BatchTag tag,
                                int elementStage, bool isType3);

  static void End(const ExecBatchContext &ctx);

  static void ResetFrameCaches();
  static void ResetCaches();
  static void UpdateUnitCache();

  static void SetHandleManagerAddrs(uintptr_t handleMgrBase,
                                    uintptr_t gameWar3PtrAddr);

  // [RENDER CONTEXT BRIDGE]
  // 允许高层渲染函数（如 WorldObjectEntry_Render）设置当前正在渲染的对象
  // 底层 ExecBatch 可以直接读取此上下文，绕过脆弱的内容结构猜测
  static void SetCurrentBatchEntry(void *entry);
  static void *GetCurrentBatchEntry();

  // [DEBUG PROBE] 注册 SceneNode 用于在 ExecBatch 中反向搜索
  static void DebugRegisterSceneNode(void *node);
};

} // namespace render
} // namespace war3
} // namespace dxvk
