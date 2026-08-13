#!/usr/bin/env python3
"""Contracts for lazy Stage11 CPU-readable position spans."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")


class Stage11PositionSpanLazyContracts(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        begin = DEVICE.index("War3ShadowDrawTimeCapturePhase::PositionSource")
        end = DEVICE.index("War3ShadowDrawTimeCapturePhase::FingerprintAndDedup", begin)
        cls.capture = DEVICE[begin:end]
        resolver_begin = cls.capture.index(
            "const auto resolvePositionReadableSpan = [&]() -> bool"
        )
        resolver_end = cls.capture.index(
            "// Resolve the exact IB before choosing", resolver_begin
        )
        cls.eager = cls.capture[:resolver_begin]
        cls.resolver = cls.capture[resolver_begin:resolver_end]

    def test_eager_path_keeps_gpu_source_proof_but_not_mapping(self) -> None:
        for token in (
            "m_war3PerDrawUpload.vbValid[posStream]",
            "FlushBuffer(posCommon)",
            "GetBufferSlice<D3D9_COMMON_BUFFER_TYPE_REAL>",
            "posBindingOffset = m_state.vertexBuffers[posStream].offset",
            "posCommonNeedsReadback = posCommon->NeedsReadback()",
            "drawTimeVBCacheRejectInvalidStride",
            "drawTimeVBCacheRejectNoSlice",
        ):
            self.assertIn(token, self.eager)
        self.assertNotIn("GetMappedSlice()", self.eager)
        self.assertNotIn("BuildWar3CpuReadableBufferSpan", self.eager)

    def test_resolver_is_one_shot_and_preserves_both_source_routes(self) -> None:
        self.assertLess(
            self.resolver.index("if (posReadableSpanResolveAttempted)"),
            self.resolver.index("posReadableSpanResolveAttempted = true"),
        )
        for token in (
            "if (DynamicSysmemVBOs)",
            "m_war3PerDrawUpload.vbUploadBytes[posStream]",
            "m_war3PerDrawUpload.vbSourceContentGeneration[posStream]",
            "posCommon == nullptr || posCommonNeedsReadback",
            "posMappedAllocation = posCommon->GetMappedSlice()",
            "posMappedAllocation->getBufferInfo()",
            "posMappedAllocation->mapPtr()",
            "posCommon->War3IdentityGeneration()",
            "posCommon->War3MapAllocationGeneration()",
            "posCommon->War3ContentGeneration()",
        ):
            self.assertIn(token, self.resolver)

    def test_package_and_marker_are_the_only_call_sites(self) -> None:
        self.assertEqual(self.capture.count("resolvePositionReadableSpan();"), 2)
        package_call = self.capture.index(
            "if (persistentPackageObserveEnabled)\n"
            "          resolvePositionReadableSpan();"
        )
        marker_gate = self.capture.index(
            "War3LegacyDrawIsPathBlockerGeometryMarkerCandidate(markerProbe"
        )
        marker_call = self.capture.index("resolvePositionReadableSpan();", marker_gate)
        marker_bounds = self.capture.index(
            "War3ComputeMappedLocalBoundsFromBytes(", marker_call
        )
        self.assertLess(package_call, marker_gate)
        self.assertLess(marker_gate, marker_call)
        self.assertLess(marker_call, marker_bounds)

    def test_normal_copy_and_generation_proof_do_not_depend_on_span(self) -> None:
        proof_begin = self.capture.index("GenerationBackedStreamProof currentPositionSourceProof")
        proof_end = self.capture.index("War3ShadowDrawTimeCapturePhase::MarkerGatesAndBounds")
        proof = self.capture[proof_begin:proof_end]
        self.assertNotIn("posReadableSpan", proof)
        self.assertNotIn("posCanonicalBytes", proof)


if __name__ == "__main__":
    unittest.main()
