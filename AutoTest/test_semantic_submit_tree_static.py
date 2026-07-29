#!/usr/bin/env python3
"""Static contracts for the low-overhead Populate/Submit timing tree."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
DEVICE = ROOT / "src" / "d3d9" / "d3d9_device.cpp"


class SemanticSubmitTreeStaticTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = DEVICE.read_text(encoding="utf-8")

    def test_exact_attempt_and_loop_timers_exist(self) -> None:
        self.assertIn("submitAppendExactAttemptTicks", self.source)
        self.assertIn("submitAppendLoopTicks", self.source)
        self.assertIn(
            "submitAppendLoopTicks > submitAppendExactAttemptTicks",
            self.source,
        )

    def test_sampled_top_level_is_normalized_to_exact_attempt(self) -> None:
        self.assertIn(
            "double(submitAppendExactAttemptTicks) /\n"
            "          double(sampledAppendPhaseTicks)",
            self.source,
        )
        self.assertIn(
            '"SubmitAppend/AttemptWork", kSubmitAppendPhaseNames[i]',
            self.source,
        )

    def test_nested_trees_close_to_normalized_parents(self) -> None:
        self.assertIn("fastNestedNormalize", self.source)
        self.assertIn("bookkeepingNestedNormalize", self.source)
        self.assertIn("fallbackNormalize", self.source)
        self.assertIn(
            '"SubmitAppend/AttemptWork/FallbackAppend/PaletteIndex/"\n'
            '            "PaletteStorage"',
            self.source,
        )
        self.assertIn('"Stats/ObjectKind"', self.source)
        self.assertIn('"Stats/DynamicEvidence"', self.source)
        self.assertIn('"GenericObjectKindReuse"', self.source)
        self.assertIn('"GenericDynamicEvidenceRecompute"', self.source)
        self.assertIn('"GenericDynamicEvidenceSkipped"', self.source)

    def test_publish_work_is_outside_submit_append_scope(self) -> None:
        close_index = self.source.index(
            "// Close the real SubmitAppend scope before publishing"
        )
        reset_index = self.source.index("directDetailScope.reset();", close_index)
        publish_index = self.source.index(
            "submitBreakdownPublishBegin", reset_index
        )
        self.assertLess(reset_index, publish_index)
        self.assertIn('"SubmitBreakdownPublish"', self.source[publish_index:])

    def test_misleading_old_sample_names_are_gone(self) -> None:
        for old_name in (
            "SampleFastAttempt",
            "SampleFallbackAppend",
            "SampleBookkeeping",
        ):
            self.assertNotIn(old_name, self.source)

    def test_fast_append_stats_reuses_proven_unit_gate(self) -> None:
        self.assertIn(
            "fastAppended && War3SemanticFastAppendStatsReuseRuntime()",
            self.source,
        )
        self.assertIn(
            "successful fast append did not resolve to Unit",
            self.source,
        )
        self.assertIn(
            "if (!reuseFastAppendStats) {",
            self.source,
        )

    def test_fast_append_stats_reuse_has_exact_runtime_rollback(self) -> None:
        self.assertIn(
            '"DXVK_WAR3_SEMANTIC_FAST_APPEND_STATS_REUSE", 1u',
            self.source,
        )
        self.assertIn(
            '"DXVK_WAR3_SEMANTIC_FAST_APPEND_STATS_REUSE_VERIFY", 0u',
            self.source,
        )

    def test_generic_append_stats_reuses_published_and_eligible_facts(self) -> None:
        self.assertIn("casterCountBeforeAppend", self.source)
        self.assertIn("appendedObjectKindKnown", self.source)
        self.assertIn("eligible.statsDynamicUnitEvidenceKnown = true", self.source)
        self.assertIn("reuseGenericAppendStats", self.source)
        self.assertIn("reuseGenericDynamicEvidence", self.source)

    def test_generic_append_stats_reuse_has_rollback_and_verifier(self) -> None:
        self.assertIn(
            '"DXVK_WAR3_SEMANTIC_GENERIC_APPEND_STATS_REUSE", 1u',
            self.source,
        )
        self.assertIn(
            '"DXVK_WAR3_SEMANTIC_GENERIC_APPEND_STATS_REUSE_VERIFY", 0u',
            self.source,
        )
        self.assertIn(
            "published caster object kind disagrees with legacy resolver",
            self.source,
        )
        self.assertIn(
            "eligibility evidence disagrees with legacy predicate",
            self.source,
        )

    def test_exact_dynamic_evidence_is_diagnostics_only_and_opt_in(self) -> None:
        self.assertIn(
            '"DXVK_WAR3_SEMANTIC_DYNAMIC_EVIDENCE_STATS", 0u',
            self.source,
        )
        self.assertIn("collectExactDynamicEvidence", self.source)
        self.assertIn("GenericDynamicEvidenceSkipped", self.source)


if __name__ == "__main__":
    unittest.main()
