from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")
HEADER = (
    ROOT
    / "src/d3d9/war3/memory/war3_coherent_real_index_trim_contract.h"
).read_text(encoding="utf-8")
ARENA_H = (ROOT / "src/d3d9/war3/memory/war3_shadow_arena.h").read_text(
    encoding="utf-8"
)
OPTIONS = (ROOT / "meson_options.txt").read_text(encoding="utf-8")
MESON = (ROOT / "src/d3d9/meson.build").read_text(encoding="utf-8")
DIAG = (ROOT / "src/d3d9/war3/tools/war3_diagnostics_hub.cpp").read_text(
    encoding="utf-8"
)
PERF_H = (ROOT / "src/d3d9/war3/tools/war3_perf_monitor.h").read_text(
    encoding="utf-8"
)
PERF_CPP = (ROOT / "src/d3d9/war3/tools/war3_perf_monitor.cpp").read_text(
    encoding="utf-8"
)


def function_body(text: str, signature: str) -> str:
    start = text.index(signature)
    brace = text.index("{", start)
    depth = 0
    for pos in range(brace, len(text)):
        if text[pos] == "{":
            depth += 1
        elif text[pos] == "}":
            depth -= 1
            if depth == 0:
                return text[brace : pos + 1]
    raise AssertionError(f"unterminated function: {signature}")


class CoherentRealIndexTrimStaticTest(unittest.TestCase):
    def test_release_default_is_unreachable(self):
        self.assertIn(
            "option('warvk_coherent_real_index_trim_dev', type : 'boolean', value : false",
            OPTIONS,
        )
        self.assertIn("kCoherentRealIndexTrimDevelopmentEnabled = false", HEADER)
        self.assertIn("get_option('warvk_coherent_real_index_trim_dev')", MESON)
        runtime = function_body(DEVICE, "War3CoherentRealIndexTrimModeRuntime()")
        self.assertIn("kCoherentRealIndexTrimDevelopmentEnabled", runtime)
        self.assertIn("War3CoherentRealIndexTrimMode::Off", runtime)

    def test_contract_is_current_rigid_opaque_terrain_only(self):
        body = function_body(HEADER, "EvaluateWar3CoherentRealIndexTrim(")
        for token in (
            "indexedTerrain",
            "legacyGpuSkin",
            "vertexBlendEnabled",
            "alphaTestEnabled",
            "alphaBlendEnabled",
            "dynamicRealPosition",
            "currentPositionSpan",
            "currentIndexSpan",
            "IndexRangeOutsideSpan",
            "PositionRangeOutsideSpan",
        ):
            self.assertIn(token, body)
        self.assertNotIn("fingerprint", HEADER.lower())

    def test_position_span_and_rebased_index_are_immediate_cpu_snapshots(self):
        body = function_body(DEVICE, "void D3D9DeviceEx::War3TryCaptureShadowCaster(")
        position_proof = body.index("coherentRealPositionSpanStillCurrent = true")
        mutation = body.index("const bool consumeCoherentRealTrim")
        freeze_plan = body.index("struct FrameFreezePlan")
        self.assertLess(position_proof, mutation)
        self.assertLess(mutation, freeze_plan)
        self.assertIn("s_exactRebasedIndexScratch.data()", body)
        self.assertIn("immediateCpuSnapshot", body)
        self.assertIn(
            "coherentRealPositionSpan.data + size_t(posFreezeByteOffset)", body
        )
        self.assertNotIn("s_coherentRealPositionScratch", body)
        self.assertIn("coherentRealIndexRangeSnapshot", body)
        self.assertIn("coherentRealIndexBytes = exactIndexSpan.data", body)
        self.assertIn("currentIndexMapping.ptr() ==", body)
        self.assertIn("currentMapping.ptr() ==", body)
        self.assertIn("coherentRealPositionMappedAllocation.ptr()", body)
        self.assertIn("War3IdentityGeneration() ==", body)
        self.assertIn("War3MapAllocationGeneration() ==", body)
        self.assertIn("War3ContentGeneration() ==", body)

    def test_consume_reduces_admission_before_arena_transaction(self):
        body = function_body(DEVICE, "void D3D9DeviceEx::War3TryCaptureShadowCaster(")
        consume = body.index("coherentRealTrimConsumed = consumeCoherentRealTrim")
        budget = body.index("const uint64_t fallbackPosBudgetBytes")
        reserve = body.index("ShadowArena_BeginBundle(")
        self.assertLess(consume, budget)
        self.assertLess(budget, reserve)
        self.assertIn("posBytesNeeded = VkDeviceSize(count * posStride)", body)

    def test_diagnostics_are_bounded_counters(self):
        for token in (
            "coherentRealTrimObservedCount",
            "coherentRealTrimEligibleCount",
            "coherentRealTrimWouldSaveBytes",
            "coherentRealTrimConsumedCount",
            "coherentRealTrimConsumedBytesSaved",
        ):
            self.assertIn(token, ARENA_H)
            self.assertIn(token, DIAG)

        for token in (
            "coherentRealTrimObservedCountLast",
            "coherentRealTrimEligibleCountLast",
            "coherentRealTrimWouldSaveBytesLast",
            "coherentRealTrimConsumedCountLast",
            "coherentRealTrimConsumedBytesSavedLast",
        ):
            self.assertIn(token, PERF_H)
            self.assertIn(token, PERF_CPP)


if __name__ == "__main__":
    unittest.main()
