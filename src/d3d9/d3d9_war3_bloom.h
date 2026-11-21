#pragma once

#include "d3d9_war3_settings.h"
#include "war3/effects/war3_bloom_executor.h"

namespace dxvk {

class D3D9DeviceEx;

// Post-processing (bloom, exposure, tone map) in process. Not a pipeline pass:
// the device runs it after the BeforeUi passes, on the world colour target,
// while the UI has not started (D3D9DeviceEx::War3MaybeInsertBeforeUi). The
// chain itself is the shared executor (war3/effects/war3_bloom_executor.h).
class War3BloomPass {
public:
  explicit War3BloomPass(D3D9DeviceEx* device);

  // The record the settings describe: bloom off still composites exposure and
  // tone map, as the D3D9 implementation did.
  static war3::effects::scene::Bloom Record(const War3PostFxSettings& settings);

  war3::effects::BloomStage Run(const Rc<DxvkCommandList>& ctx,
                                const Rc<DxvkImageView>& color,
                                const war3::effects::scene::Bloom& record);

private:
  war3::effects::BloomExecutor m_executor;
};

} // namespace dxvk
