"""Static contracts for exact current-frame Stage11 attachment recovery."""

from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
DEVICE = ROOT / "src/d3d9/d3d9_device.cpp"
SHADOW = ROOT / "src/d3d9/d3d9_war3_shadow.cpp"
HOOK = ROOT / "src/d3d9/war3/hooks/war3_hook_render.cpp"
CONTRACT = ROOT / "src/d3d9/war3/render/war3_current_draw_contract.h"


def source_block(text: str, start: str, end: str, offset: int = 0) -> str:
    begin = text.index(start, offset)
    finish = text.index(end, begin)
    return text[begin:finish]


class Stage11ExactAttachmentFallbackStaticTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.device = DEVICE.read_text(encoding="utf-8")
        cls.shadow = SHADOW.read_text(encoding="utf-8")
        cls.hook = HOOK.read_text(encoding="utf-8")
        cls.contract = CONTRACT.read_text(encoding="utf-8")
        cls.binding = source_block(
            cls.device,
            "struct War3CurrentDrawCaptureBinding {",
            "bool War3CurrentDrawContractMatchesRenderableInstance(",
        )
        capture_start = cls.device.index(
            "void D3D9DeviceEx::War3TryCaptureShadowCaster("
        )
        cls.capture = cls.device[capture_start:]

    def test_geometry_identity_comes_from_native_child_dispatch(self) -> None:
        for token in (
            "out.renderablePart = dispatch.renderablePart;",
            "out.sceneNode = dispatch.sceneNode;",
            "out.layerIndex = dispatch.layerIndex;",
            "semantic.renderablePart != dispatch.renderablePart",
        ):
            self.assertIn(token, self.binding)

        # A parent/child bridge needs a real owner witness. Model names,
        # rawcodes and shared resource pointers are not instance identities.
        self.assertIn("semantic.worldObjectEntry != nullptr", self.binding)
        self.assertIn("semanticUnitPtr != nullptr", self.binding)
        self.assertIn("semanticJHandle != 0u", self.binding)
        owner_gate = source_block(
            self.binding,
            "if (!out.attachmentBridge)",
            "bool War3CurrentDrawContractMatchesCaptureBinding(",
        )
        self.assertNotIn("rawcode", owner_gate)
        self.assertNotIn("modelKey", owner_gate)

    def test_child_contract_does_not_recompare_parent_scene_node(self) -> None:
        match = source_block(
            self.device,
            "bool War3CurrentDrawContractMatchesCaptureBinding(",
            "bool War3CurrentDrawContractMatchesRenderableInstance(",
        )
        self.assertIn("contract.renderablePart != binding.renderablePart", match)
        self.assertIn("binding.sceneNode != contract.sceneNode", match)
        self.assertIn("if (!binding.attachmentBridge)", match)
        attachment = match[match.index("if (!binding.attachmentBridge)") :]
        self.assertNotIn("semantic.sceneNode != contract.sceneNode", attachment)
        self.assertIn("return ownerMatched;", attachment)

    def test_metadata_and_geometry_use_the_same_exact_child_slice(self) -> None:
        metadata = source_block(
            self.device,
            "bool D3D9DeviceEx::War3CaptureShadowDrawMetadata(",
            "void D3D9DeviceEx::War3TryCaptureShadowCaster(",
        )
        for token in (
            "War3ResolveCurrentDrawCaptureBinding(",
            "renderablePart, captureBinding.sceneNode",
            "renderablePart, captureBinding.layerIndex, contract",
            "War3CurrentDrawContractMatchesCaptureBinding(",
        ):
            self.assertIn(token, metadata)

        exact = source_block(
            self.capture,
            "War3ShadowDrawTimeCapturePhase::IdentityResolve",
            "War3ShadowDrawTimeCapturePhase::PositionSource",
        )
        for token in (
            "captureBinding.sceneNode",
            "const uint32_t vbCacheLayerIndex = captureBinding.layerIndex;",
            "War3CurrentDrawContractMatchesCaptureBinding(",
        ):
            self.assertIn(token, exact)

    def test_fixed_function_child_never_enters_rigid_producer(self) -> None:
        route = source_block(
            self.capture,
            "const DWORD nativeVertexBlendState =",
            "const bool gpuSkinSemanticOutputHasUv =",
        )
        for token in (
            "captureBinding.attachmentBridge &&",
            "transparentType0Exact || nativeFixedFunctionBlend",
            "!gpuSkinSemanticBacking && !gpuSkinSemanticInputExact",
            "stage11AttachmentLegacyFallback = true;",
            "stage11AttachmentCaptureContract = vbCacheContract;",
            "gpuSkinSemanticFallbackToLegacy = true;",
            "break;",
        ):
            self.assertIn(token, route)

        producer = source_block(
            self.device,
            "uint32_t D3D9DeviceEx::War3TryPopulateDrawTimeSemanticProducer()",
            "bool D3D9DeviceEx::War3DrainShadowCasterTombstones()",
        )
        self.assertIn("draw.vertexBlendEnabled = false;", producer)
        self.assertIn("draw.vertexBlendIndexed = false;", producer)

    def test_fallback_is_same_frame_exact_and_preserves_child_identity(self) -> None:
        self.assertIn(
            "if (earlySemanticSceneUnitLikeCandidate &&\n"
            "      !stage11AttachmentLegacyFallback)",
            self.capture,
        )
        self.assertIn(
            "!stage11AttachmentLegacyFallback)",
            source_block(
                self.capture,
                "if (vertexBlendEnabled && semantic.runtimeModelPtr",
                "// 说明：batchHandle",
            ),
        )
        draw = source_block(
            self.capture,
            "War3ShadowCasterDraw draw = {};",
            "draw.indexed = indexed;",
            self.capture.index("War3ShadowCasterDraw draw = {};", 40000),
        )
        for token in (
            "draw.mapEpoch = m_war3GpuSkinMapEpoch;",
            "draw.deviceEpoch = m_war3GpuSkinDeviceEpoch;",
            "stage11AttachmentCaptureBinding.renderablePart",
            "stage11AttachmentCaptureBinding.layerIndex",
            "stage11AttachmentCaptureContract",
            "War3ShadowPartLifecycleState::RequiredCurrent",
        ):
            self.assertIn(token, draw)

        self.assertIn(
            'War3GetEnvU32("DXVK_WAR3_DRAWTIME_VB_CACHE", 0u)',
            self.device,
        )
        self.assertIn(
            'War3GetEnvU32("DXVK_WAR3_DRAWTIME_CURRENT_FRAME_GEOMETRY", 1u)',
            self.device,
        )

    def test_type0_indexed_blend_requires_indices_but_not_weights(self) -> None:
        validation = source_block(
            self.shadow,
            "validation.blendRequired = draw.vertexBlendEnabled",
            "if (validation.gpuSkinRequired)",
        )
        self.assertIn(
            "draw.vertexBlendCount != 0u || draw.vertexBlendIndexed",
            validation,
        )
        self.assertIn(
            "const uint32_t weightEnd = draw.vertexBlendCount != 0u",
            validation,
        )
        self.assertIn("const uint32_t indexEnd = draw.vertexBlendIndexed", validation)

    def test_transparent_type0_is_a_resident_unknown_layer_scope(self) -> None:
        for token in (
            "TransparentType0 = 3u",
            "kRenderQueueUnknownLayerIndex",
            "bool layerKnown = false;",
            "void* meshPayload = nullptr;",
        ):
            self.assertIn(token, self.contract)

        type0 = source_block(
            self.hook,
            "void __fastcall Hook_TransparentDispatchType0Semantic",
            "void __fastcall Hook_TransparentDispatchType1Timing",
        )
        for token in (
            "SafeReadPtrFast(batch, 0x0cu, meshPayload)",
            "CurrentDrawDispatchDomain::TransparentType0",
            "kRenderQueueUnknownLayerIndex",
            "false, meshPayload",
        ):
            self.assertIn(token, type0)

        install = source_block(
            self.hook,
            "// Type0 is a correctness boundary",
            "if (War3TransparentDispatchTimingHooksRuntimeEnabled())",
        )
        self.assertIn("Hook_TransparentDispatchType0Semantic", install)
        self.assertNotIn("DXVK_WAR3_PERF_TRANSPARENT_DISPATCH_HOOKS", install)

    def test_type0_uses_value_only_same_frame_contract(self) -> None:
        exact = source_block(
            self.capture,
            "const bool transparentType0Exact =",
            "War3ShadowDrawTimeCapturePhase::PositionSource",
        )
        for token in (
            "vbCacheContract.meshPayloadPtr = captureBinding.meshPayload;",
            "kRenderQueueUnknownLayerIndex",
            "vbCacheContract.producerFreshThisFrame = true;",
            "vbCacheContract.renderFrameIndex = currentRenderFrameIndex;",
            "if (!transparentType0Exact &&",
        ):
            self.assertIn(token, exact)
        synthetic = source_block(
            exact,
            "if (transparentType0Exact) {",
            "} else {",
        )
        self.assertNotIn("PublishCurrentDrawContract", synthetic)
        self.assertNotIn("QueryCurrentDrawContract", synthetic)

    def test_building_type0_alpha_exception_is_narrow(self) -> None:
        route = source_block(
            self.capture,
            "const bool buildingOwner =",
            "gpuSkinSemanticFallbackToLegacy = true;",
        )
        for token in (
            "ObjectKind::Building",
            "transparentType0Exact && buildingOwner",
            "D3DRS_ZWRITEENABLE",
        ):
            self.assertIn(token, route)
        alpha = source_block(
            self.capture,
            "if (alphaBlend) {",
            "War3ShadowCapturePostPhase::VertexLayout",
        )
        self.assertIn("if (isAdditive)", alpha)
        self.assertIn(
            "if (!alphaTestEnabled && !stage11AttachmentAllowOpaqueBlend)",
            alpha,
        )


if __name__ == "__main__":
    unittest.main()
