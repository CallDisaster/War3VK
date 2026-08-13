#!/usr/bin/env python3
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
DEVICE_CPP = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(
    encoding="utf-8", errors="replace"
)


def braced_block(source: str, marker: str, start: int = 0) -> str:
    marker_pos = source.index(marker, start)
    open_pos = source.index("{", marker_pos)
    depth = 0
    for pos in range(open_pos, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[marker_pos : pos + 1]
    raise AssertionError(f"unterminated block after {marker}")


class Stage13RetentionLazyHashStaticTest(unittest.TestCase):
    def test_world_and_material_hashes_are_base_eligible_only(self) -> None:
        begin = DEVICE_CPP.index("uint64_t stage13WorldMatrixHash = 0u;")
        gate = braced_block(
            DEVICE_CPP, "if (stage13BaseRetentionEligible)", begin
        )
        for token in (
            "stage13WorldMatrixHash = bit::fnv1a_init();",
            "bit::cast<uint32_t>(shadowWorldMatrix[col][row])",
            "stage13MaterialHash = bit::fnv1a_init();",
            "GetCommonTexture(m_state.textures[0])",
            "m_state.renderStates[D3DRS_CULLMODE]",
        ):
            self.assertIn(token, gate)

        end = DEVICE_CPP.index("const auto buildStage13RetentionKey", begin)
        region = DEVICE_CPP[begin:end]
        outside_gate = region.replace(gate, "", 1)
        self.assertNotIn("GetCommonTexture(", outside_gate)
        self.assertNotIn(
            "stage13WorldMatrixHash = bit::fnv1a_init()", outside_gate
        )

    def test_canonical_content_hash_is_base_eligible_only(self) -> None:
        begin = DEVICE_CPP.index(
            "uint64_t stage13CanonicalReferencedContentHash = 0u;"
        )
        end = DEVICE_CPP.index(
            "const bool stage13CanonicalReferencedContentHashValid", begin
        )
        block = DEVICE_CPP[begin:end]
        gate = braced_block(block, "if (stage13BaseRetentionEligible)")
        self.assertIn("computeStage13ReferencedContentHash(", gate)
        self.assertIn("stage13CanonicalPositionBytes", gate)
        self.assertIn("stage13CanonicalIndexBytes", gate)
        self.assertEqual(block.count("computeStage13ReferencedContentHash("), 1)

    def test_late_content_hash_is_required_identity_only(self) -> None:
        begin = DEVICE_CPP.index(
            "bool stage13ReferencedPositionEligible = false;"
        )
        end = DEVICE_CPP.index(
            "War3ShadowStage13StrongPhase::VerifyAndLookup", begin
        )
        block = DEVICE_CPP[begin:end]
        gate = braced_block(
            block, "if (stage13NeedsReferencedPositionIdentity)"
        )
        self.assertIn("computeStage13ReferencedContentHash(", gate)
        self.assertIn("stage13MappedPositionBytes", gate)
        self.assertIn("stage13MappedIndexBytes", gate)
        self.assertEqual(block.count("computeStage13ReferencedContentHash("), 1)

    def test_stage13_release_defaults_remain_disabled(self) -> None:
        for token in (
            '"DXVK_WAR3_STAGE13_STATIC_RETENTION", 0u',
            '"DXVK_WAR3_STAGE13_SOURCE_GENERATION_VERIFY", 0u',
            '"DXVK_WAR3_STAGE13_UNIQUE_SEMANTIC_CACHE", 0u',
            '"DXVK_WAR3_STAGE13_LATE_DESCRIPTOR_CACHE", 0u',
        ):
            self.assertIn(token, DEVICE_CPP)


if __name__ == "__main__":
    unittest.main()
