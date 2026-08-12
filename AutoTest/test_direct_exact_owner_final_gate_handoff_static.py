"""Lock the same-frame exact-owner proof handoff into DirectGrouped append."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")


class DirectExactOwnerFinalGateHandoffStaticTests(unittest.TestCase):
    def test_only_canonical_preselection_publishes_the_proof(self) -> None:
        build = DEVICE[
            DEVICE.index("for (size_t buildIndex = 0u;") :
            DEVICE.index("War3BuildEligibleRecordRawTiming", DEVICE.index("for (size_t buildIndex = 0u;"))
        ]
        self.assertIn(
            "eligible.currentFrameExactOwnerPrefiltered =\n"
            "        recordsForBuildCanonicalPrefiltered && !useSealedWork",
            DEVICE,
        )
        self.assertIn(
            "!recordsForBuildCanonicalPrefiltered && !useSealedWork",
            build,
        )

    def test_final_gate_is_skipped_only_by_the_carried_value_proof(self) -> None:
        start = DEVICE.index(
            "bool D3D9DeviceEx::War3TryAppendSemanticShadowPacket(\n"
            "    const dxvk::war3::shadow::ShadowDrawPacket& packet,\n"
            "    const dxvk::war3::render::CurrentDrawAuthoritativeSample*\n"
            "        directCurrentDrawSample,\n"
            "    bool fromStalePoseRestore,\n"
            "    bool currentFrameExactOwnerPrefiltered)"
        )
        end = DEVICE.index("War3FallbackAppendRawTiming", start)
        gate = DEVICE[start:end]
        self.assertIn(
            "if (!currentFrameExactOwnerPrefiltered &&\n"
            "      War3DrawTimeCurrentFrameGeometryRuntime()",
            gate,
        )
        for token in (
            "War3DrawTimeAnonymousMarkerRejectionActive(",
            "War3MakeDrawTimeVBCacheKey(",
            "War3DrawTimeExactRejectedCurrentFrame(cacheKey)",
            "m_war3DrawTimeVBCache.find(cacheKey)",
            "vbIt->second.MatchesKey(cacheKey)",
            "vbIt->second.exactOwnerFrameSerial ==",
        ):
            self.assertIn(token, gate)

    def test_restored_and_alternate_callers_keep_the_defensive_gate(self) -> None:
        wrapper_start = DEVICE.index(
            "bool D3D9DeviceEx::War3TryAppendSemanticShadowPacket(\n"
            "    const dxvk::war3::shadow::ShadowDrawPacket& packet,\n"
            "    const dxvk::war3::render::CurrentDrawAuthoritativeSample*\n"
            "        directCurrentDrawSample,\n"
            "    bool fromStalePoseRestore)"
        )
        wrapper_end = DEVICE.index("bool D3D9DeviceEx::War3TryAppendSemanticShadowPacket(", wrapper_start + 10)
        self.assertIn(
            "packet, directCurrentDrawSample, fromStalePoseRestore, false",
            DEVICE[wrapper_start:wrapper_end],
        )
        append = DEVICE[
            DEVICE.index("auto tryAppendEligibleAndNote =") :
            DEVICE.index("if (!useObjectGrouped)", DEVICE.index("auto tryAppendEligibleAndNote ="))
        ]
        self.assertIn("eligible.currentFrameExactOwnerPrefiltered", append)


if __name__ == "__main__":
    unittest.main()
