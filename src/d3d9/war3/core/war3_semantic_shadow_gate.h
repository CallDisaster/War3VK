#pragma once

namespace dxvk::war3::internal {

bool IsSemanticShadowPreviewEnabled();
bool IsSemanticCoreValidationRuntimeEnabled();
bool IsSemanticSceneSubmissionRuntimeEnabled();
bool IsSemanticSceneBootstrapCatchupRuntimeEnabled();
bool IsSemanticSceneEndFrameBuildRuntimeEnabled();
bool IsSemanticSceneEndFrameFlushRuntimeEnabled();
bool IsSemanticSceneTailBoundaryFallbackRuntimeEnabled();
bool IsSemanticSceneBypassLegacyUnitCaptureRuntimeEnabled();
bool IsSemanticSceneDisableLegacyShadowCaptureRuntimeEnabled();
bool IsSemanticShadowPreReadyValidationRuntimeEnabled();
bool IsNativeRendererHostExecuteValidationRuntimeEnabled();
bool IsNativeSemanticShadowWorldStageValidationRuntimeEnabled();

} // namespace dxvk::war3::internal
