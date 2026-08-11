#!/usr/bin/env python3
"""Contracts for generation-backed Stage11 static-stream reuse."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")
DEVICE_H = (ROOT / "src/d3d9/d3d9_device.h").read_text(encoding="utf-8")
SCENE = (ROOT / "src/d3d9/d3d9_war3_scene.h").read_text(encoding="utf-8")
PROOF = (ROOT / "src/d3d9/war3/render/war3_shadow_generation_backed_stream.h").read_text(encoding="utf-8")
RUNTIME = (ROOT / "src/d3d9/war3/render/war3_shadow_runtime_bridge.cpp").read_text(encoding="utf-8")
PERF = (ROOT / "src/d3d9/war3/tools/war3_perf_monitor.cpp").read_text(encoding="utf-8")


class GenerationBackedCacheContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        begin = DEVICE.index("using GenerationBackedStreamProof")
        end = DEVICE.index("entry.lastCaptureFingerprint = captureFingerprint", begin)
        cls.capture = DEVICE[begin:end]

    def test_value_proof_uses_generations_range_and_epochs(self) -> None:
        for token in (
            "ownerIdentity",
            "identityGeneration",
            "allocationGeneration",
            "contentGeneration",
            "sourceOffset",
            "sourceLength",
            "mapEpoch",
            "deviceEpoch",
            "streamKind",
        ):
            self.assertIn(token, PROOF)
        for forbidden in (
            "lastCaptureFingerprint",
            "sourceFingerprint",
            "std::hash",
            "getenv",
            "mutex",
        ):
            self.assertNotIn(forbidden, PROOF)

    def test_stage11_proof_comes_from_authoritative_generations(self) -> None:
        for token in (
            "War3IdentityGeneration()",
            "War3MapAllocationGeneration()",
            "War3ContentGeneration()",
            "vbSourceIdentityGeneration[stream]",
            "vbSourceSequence[stream]",
            "vbSourceContentGeneration[stream]",
            "ibSourceIdentityGeneration",
            "ibSourceSequence",
            "ibSourceContentGeneration",
        ):
            self.assertIn(token, self.capture)

    def test_all_three_copies_require_exact_matching_proof(self) -> None:
        for stream in ("Position", "Uv", "Index"):
            self.assertIn(f"generationBacked{stream}Reuse", self.capture)
            self.assertIn(f"{stream.lower()}SourceProof.matches", self.capture)
        self.assertIn("!generationBackedPositionReuse", self.capture)
        self.assertIn("!generationBackedUvReuse", self.capture)
        self.assertIn("!generationBackedIndexReuse", self.capture)
        self.assertIn("drawTimeGenerationBackedCopyBytesSaved", self.capture)

    def test_dynamic_and_gpu_skin_routes_cannot_reuse_static_streams(self) -> None:
        candidate = self.capture[
            self.capture.index("const bool generationBackedStaticCandidate") :
            self.capture.index("const auto makeDirectStreamProof")
        ]
        self.assertIn("!isDynamicUnit", candidate)
        self.assertIn("!gpuSkinSemanticBacking", candidate)
        self.assertIn("!gpuSkinSemanticDirectOnly", candidate)
        clear = self.capture[
            self.capture.index("if (generationBackedStaticCandidate)") :
            self.capture.index("War3ShadowDrawTimeCapturePhase::FinalizeAccounting")
        ]
        self.assertIn("entry.positionSourceProof = {};", clear)
        self.assertIn("entry.indexSourceProof = {};", clear)
        self.assertIn("entry.uvSourceProof = {};", clear)

    def test_legacy_fingerprint_is_not_the_new_authority(self) -> None:
        runtime = DEVICE[DEVICE.index("inline bool War3DrawTimeSourceFingerprintReuseRuntime") :]
        runtime = runtime[: runtime.index("}\n", runtime.index("{")) + 2]
        self.assertIn("kReleaseFreezeExperimentalShadowRoutes", runtime)
        self.assertIn("War3DrawTimeSourceFingerprintReuseRuntime", DEVICE)
        self.assertNotIn("lastCaptureFingerprint", PROOF)
        self.assertIn("positionSourceProof", DEVICE_H)

    def test_diagnostics_reach_runtime_and_performance_reports(self) -> None:
        for name in (
            "drawTimeGenerationBackedPositionReuseCount",
            "drawTimeGenerationBackedUvReuseCount",
            "drawTimeGenerationBackedIndexReuseCount",
            "drawTimeGenerationBackedCopyBytesSaved",
        ):
            self.assertIn(name, SCENE)
            self.assertIn(name, RUNTIME)
            self.assertIn(name, PERF)


if __name__ == "__main__":
    unittest.main()
