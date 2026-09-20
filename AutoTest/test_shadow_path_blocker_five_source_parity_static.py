"""Semantic-core and NativeD3D9 path-blocker five-source parity contracts."""

from __future__ import annotations

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
EVIDENCE_H = ROOT / "src/d3d9/war3/war3_path_blocker_evidence.h"
SEMANTIC_CPP = ROOT / "src/d3d9/war3/shadow/war3_shadow_renderer_core.cpp"
NATIVE_CPP = ROOT / "src/d3d9/war3/shadow/war3_shadow_native_runtime.cpp"
CONFIG_H = ROOT / "src/d3d9/war3/core/war3_internal_test_config.h"


def function_block(source: str, signature: str, next_signature: str) -> str:
    begin = source.index(signature)
    end = source.index(next_signature, begin)
    return source[begin:end]


class PathBlockerFiveSourceParityStaticTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.evidence = EVIDENCE_H.read_text(encoding="utf-8")
        cls.semantic = SEMANTIC_CPP.read_text(encoding="utf-8")
        cls.native = NATIVE_CPP.read_text(encoding="utf-8")
        cls.config = CONFIG_H.read_text(encoding="utf-8")
        cls.resolve = function_block(
            cls.evidence,
            "inline bool PathBlockerTryResolveRawcode",
            "inline bool PathBlockerPacketCanUseBelowGroundFlatMarkerFallback",
        )
        cls.submit = function_block(
            cls.evidence,
            "inline bool PathBlockerShouldSubmitPacket",
            "} // namespace dxvk::war3",
        )
        cls.semantic_submit = function_block(
            cls.semantic,
            "bool SemanticCoreShouldSubmitResolvedPacket",
            "ShadowRenderableRecord ConvertVisibleRecord",
        )
        cls.native_submit = function_block(
            cls.native,
            "bool CanonicalShouldSubmitPacket",
            "} // namespace",
        )

    def test_widget_magic_and_rawcode_offsets_match(self) -> None:
        begin = self.evidence.index(
            "inline bool PathBlockerTryReadWidgetRawcode"
        )
        end = self.evidence.index(
            "inline bool PathBlockerTryResolveRawcode", begin
        )
        block = self.evidence[begin:end]
        self.assertIn("SafeReadU32Fast(widgetPtr, 0x0Cu, magic)", block)
        self.assertIn("magic != 0x2B5DB42Cu", block)
        self.assertIn("SafeReadU32Fast(widgetPtr, 0x30u, rawcode)", block)
        self.assertIn("IsPathBlockerFourCc(rawcode)", block)
        self.assertIn("PathBlockerTryReadWidgetRawcode", self.semantic)
        self.assertIn("PathBlockerTryReadWidgetRawcode", self.native)

    def test_four_identity_sources_have_the_same_precedence(self) -> None:
        expected = (
            "IsPathBlockerFourCc(renderable.rawcode)",
            "renderable.jHandle != 0u",
            "findByHandle(",
            "renderable.worldObjectEntry",
            "renderable.unitPtr != renderable.worldObjectEntry",
            "renderable.unitPtr",
        )
        cursor = -1
        for token in expected:
            position = self.resolve.index(token, cursor + 1)
            self.assertGreater(position, cursor)
            cursor = position

    def test_marker_is_fifth_source_and_rejects_dynamic_units(self) -> None:
        begin = self.evidence.index(
            "inline bool PathBlockerPacketCanUseBelowGroundFlatMarkerFallback"
        )
        end = self.evidence.index(
            "inline bool PathBlockerPacketIsBelowGroundFlatMarkerGeometry",
            begin,
        )
        block = self.evidence[begin:end]
        self.assertIn("kPathBlockerHideEnabled", block)
        self.assertIn("kPathBlockerBelowGroundFlatMarkerGateEnabled", block)
        self.assertIn("packet.path != shadow::ShadowDrawPath::Rigid", block)
        self.assertIn("PathBlockerHasDynamicUnitEvidence(packet)", block)
        self.assertIn(
            "renderable.objectKind == render::ObjectKind::Unit", block
        )
        self.assertIn("renderable.unitPtr != nullptr", block)
        self.assertIn("ShadowDrawPath::Skinned", self.evidence)

    def test_direct_evidence_precedes_marker_and_both_precede_submission(
        self,
    ) -> None:
        resolve = self.submit.index("PathBlockerTryResolveRawcode")
        explicit = self.submit.index("if (renderable.pathBlocker)", resolve)
        marker = self.submit.index(
            "PathBlockerPacketIsBelowGroundFlatMarkerGeometry", explicit
        )
        success = self.submit.rindex("return true;")
        self.assertLess(resolve, explicit)
        self.assertLess(explicit, marker)
        self.assertLess(marker, success)
        self.assertIn(
            "PathBlockerShouldSubmitPacket", self.semantic_submit
        )
        self.assertIn("PathBlockerShouldSubmitPacket", self.native_submit)
        self.assertIn(
            "if (resolveRecord(record, resources, poses, attachments, packet, ioStats) &&",
            self.semantic,
        )
        self.assertIn(
            "SemanticCoreShouldSubmitResolvedPacket(packet, ioStats)",
            self.semantic,
        )
        self.assertIn("CanonicalShouldSubmitPacket(packet,", self.native)
        self.assertIn("continue;", self.native)

    def test_full_eight_fourcc_set_and_default_gates_are_preserved(self) -> None:
        blocker_codes = (
            "0x59546162u",
            "0x59546163u",
            "0x59547062u",
            "0x59547063u",
            "0x59546662u",
            "0x59546663u",
            "0x59546C62u",
            "0x59546C63u",
        )
        for code in blocker_codes:
            self.assertEqual(self.config.count(code), 1)
        self.assertIn(
            "inline bool kPathBlockerHideEnabled = true;", self.config
        )
        self.assertIn(
            "kPathBlockerBelowGroundFlatMarkerGateEnabled = true;",
            self.config,
        )
        self.assertIn("kPathBlockerFourCCsCount", self.config)
        self.assertIn("NormalizePathBlockerFourCc", self.config)

    def test_callers_share_one_evidence_header(self) -> None:
        self.assertIn(
            '#include "../war3_path_blocker_evidence.h"', self.semantic
        )
        self.assertIn(
            '#include "../war3_path_blocker_evidence.h"', self.native
        )
        self.assertEqual(
            self.semantic.count("SemanticCoreShouldSubmitResolvedPacket"), 4
        )
        self.assertEqual(
            self.native.count("CanonicalShouldSubmitPacket"), 2
        )
        self.assertEqual(
            self.semantic.count("PathBlockerShouldSubmitPacket"), 1
        )
        self.assertEqual(
            self.native.count("PathBlockerShouldSubmitPacket"), 1
        )


if __name__ == "__main__":
    unittest.main()
