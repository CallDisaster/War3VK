#!/usr/bin/env python3
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
DEVICE_CPP = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(
    encoding="utf-8", errors="replace"
)
DEVICE_H = (ROOT / "src/d3d9/d3d9_device.h").read_text(
    encoding="utf-8", errors="replace"
)
PROOF_H = (
    ROOT / "src/d3d9/war3/render/war3_shadow_generation_backed_stream.h"
).read_text(encoding="utf-8", errors="replace")
OBSERVER_POLICY_H = (
    ROOT / "src/d3d9/war3/render/war3_shadow_observer_build_policy.h"
).read_text(encoding="utf-8", errors="replace")
PERF_H = (ROOT / "src/d3d9/war3/tools/war3_perf_monitor.h").read_text(
    encoding="utf-8", errors="replace"
)
PERF_CPP = (ROOT / "src/d3d9/war3/tools/war3_perf_monitor.cpp").read_text(
    encoding="utf-8", errors="replace"
)


class S1GenerationProofObserveStaticTest(unittest.TestCase):
    @staticmethod
    def _observer_block() -> str:
        marker = "Development-only observation of whether opaque S1 terrain"
        begin = DEVICE_CPP.index(marker)
        return DEVICE_CPP[
            begin : DEVICE_CPP.index("if (terrainDoodadCaster)", begin)
        ]

    def test_observer_is_value_semantic_and_two_distinct_frames(self):
        self.assertIn("War3ShadowGenerationBackedGeometryProof", PROOF_H)
        self.assertIn("War3ShadowGenerationObservationClock", PROOF_H)
        self.assertIn("AdvanceWar3ShadowGenerationObservationClock", PROOF_H)
        self.assertIn("frameSerial == state.lastObservedFrame", PROOF_H)
        self.assertIn("frameSerial != state.lastObservedFrame + 1u", PROOF_H)
        self.assertIn("state.distinctStableFrames >= requiredDistinctFrames", PROOF_H)
        self.assertIn("!state.proof.matches(proof)", PROOF_H)

    def test_exact_position_and_index_generations_are_required(self):
        for token in (
            "vbSourceIdentityGeneration",
            "vbSourceSequence",
            "vbSourceContentGeneration",
            "ibSourceIdentityGeneration",
            "ibSourceSequence",
            "ibSourceContentGeneration",
            "m_war3GpuSkinMapEpoch",
            "m_war3GpuSkinDeviceEpoch",
        ):
            self.assertIn(token, DEVICE_CPP)
        observe = self._observer_block()
        self.assertIn("s1GenerationProof.valid()", observe)
        self.assertIn("ObserveWar3ShadowGenerationStability", observe)
        self.assertIn("AdvanceWar3ShadowGenerationObservationClock", observe)
        self.assertNotIn(
            "const uint64_t observationFrame =\n            m_war3ShadowPersistentFrameSerial + 1u;",
            observe,
        )
        self.assertIn("!alphaTestEnabled && !alphaBlend", observe)

    def test_observer_does_not_enable_persistent_or_arena_consume(self):
        observe = self._observer_block()
        self.assertNotIn("War3CreateShadowPersistent", observe)
        self.assertNotIn("ShadowArena_BeginBundle", observe)
        self.assertNotIn("s1TerrainPersistentPath =", observe)
        self.assertRegex(observe, r"\(void\)s1GenerationProofPromotionReady;")

    def test_release_compiles_out_observer_gc_and_live_gauge(self):
        self.assertIn(
            "inline constexpr bool kDevelopmentShadowObserversEnabled = false;",
            OBSERVER_POLICY_H,
        )
        observe = self._observer_block()
        self.assertIn(
            "if constexpr (dxvk::war3::render::"
            "kDevelopmentShadowObserversEnabled)",
            observe,
        )
        gc = DEVICE_CPP[
            DEVICE_CPP.index("void D3D9DeviceEx::War3GcS1GenerationProofObservations") :
            DEVICE_CPP.index(
                "bool D3D9DeviceEx::War3CreateShadowPersistentBuffer",
                DEVICE_CPP.index("void D3D9DeviceEx::War3GcS1GenerationProofObservations"),
            )
        ]
        self.assertIn(
            "if constexpr (!dxvk::war3::render::"
            "kDevelopmentShadowObserversEnabled)",
            gc,
        )
        self.assertRegex(
            DEVICE_CPP,
            r"if constexpr \(dxvk::war3::render::"
            r"kDevelopmentShadowObserversEnabled\)\s+"
            r"War3GcS1GenerationProofObservations\(\);",
        )
        self.assertRegex(
            DEVICE_CPP,
            r"if constexpr \(dxvk::war3::render::"
            r"kDevelopmentShadowObserversEnabled\) \{\s+"
            r"m_war3ShadowPersistentDiagnosticsFrame\."
            r"s1GenerationProofEntryCount",
        )

    def test_observer_is_bounded_and_map_reset_clears_it(self):
        self.assertIn("kMaxS1GenerationProofObservations = 16384u", DEVICE_CPP)
        self.assertIn("War3GcS1GenerationProofObservations", DEVICE_CPP)
        reset = DEVICE_CPP[
            DEVICE_CPP.index("void D3D9DeviceEx::War3ResetShadowSessionState") :
            DEVICE_CPP.index("void D3D9DeviceEx::", DEVICE_CPP.index("void D3D9DeviceEx::War3ResetShadowSessionState") + 10)
        ]
        self.assertIn("m_war3S1GenerationProofObservations.clear()", reset)
        self.assertIn("m_war3S1GenerationProofObservationClock = {}", reset)
        self.assertIn("m_war3S1GenerationProofLastGcFrame = 0u", reset)

    def test_diagnostics_reach_perf_json(self):
        fields = (
            "s1GenerationProofEntryCount",
            "s1GenerationProofEligibleCount",
            "s1GenerationProofAdvancedCount",
            "s1GenerationProofChangedCount",
            "s1GenerationProofPromotionReadyCount",
            "s1GenerationProofCapacityRejectCount",
        )
        for field in fields:
            self.assertIn(field, DEVICE_H)
            self.assertIn(field, DEVICE_CPP)
            self.assertIn(field, PERF_H)
            self.assertIn(field, PERF_CPP)


if __name__ == "__main__":
    unittest.main()
