from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")
HEADER = (ROOT / "src/d3d9/war3/memory/war3_coherent_up_index_trim_contract.h").read_text(encoding="utf-8")
OPTIONS = (ROOT / "meson_options.txt").read_text(encoding="utf-8")
MESON = (ROOT / "src/d3d9/meson.build").read_text(encoding="utf-8")


class CoherentUpIndexTrimStaticTest(unittest.TestCase):
    def test_release_default_is_off(self):
        self.assertRegex(
            OPTIONS,
            r"warvk_coherent_up_index_trim_dev'.*?value\s*:\s*false",
        )
        self.assertIn("kCoherentUpIndexTrimDevelopmentEnabled = false", HEADER)
        self.assertIn("get_option('warvk_coherent_up_index_trim_dev')", MESON)

    def test_only_current_same_allocation_pair_is_eligible(self):
        for token in (
            "currentPositionUpload",
            "currentIndexUpload",
            "samePinnedAllocation",
            "hasPositionBytes",
            "hasIndexBytes",
            "NonZeroUploadedFirstIndex",
            "IndexRangeOutsideUpload",
            "PositionRangeOutsideUpload",
        ):
            self.assertIn(token, HEADER)

    def test_observe_and_consume_share_exact_current_bytes(self):
        self.assertIn("War3CoherentUpIndexTrimModeRuntime", DEVICE)
        self.assertIn("coherentUpTrimDecision", DEVICE)
        self.assertIn("m_war3PerDrawUpload.ibUploadBytes", DEVICE)
        self.assertIn("coherentUpTrimDecision.indexBytes", DEVICE)
        self.assertIn("coherentUpTrimCandidate ||", DEVICE)
        self.assertIn("coherentUpTrimMode == CoherentUpTrimMode::Consume", DEVICE)
        self.assertIn("ShadowArena_NoteCoherentUpIndexTrim", DEVICE)

    def test_no_cross_frame_identity_or_fingerprint_is_added(self):
        self.assertNotIn("fingerprint", HEADER.lower())
        self.assertNotIn("frame cache", HEADER.lower())


if __name__ == "__main__":
    unittest.main()
