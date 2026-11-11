#!/usr/bin/env python3
"""Static boundary checks only; the C++ companion executes the real component.

This script does not build or run native code, launch a game, or certify GPU,
integration, caller-source attribution, performance, or memory savings.
"""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "src/d3d9/d3d9_migration_census.h").read_text(encoding="utf-8")
TEST = (ROOT / "AutoTest/test_d3d9_migration_census.cpp").read_text(encoding="utf-8")
STATE = (ROOT / "src/d3d9/d3d9_state.h").read_text(encoding="utf-8")
CAPS = (ROOT / "src/d3d9/d3d9_caps.h").read_text(encoding="utf-8")


def block(source: str, start: str, end: str) -> str:
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


def code_only(source: str) -> str:
    # Static assertions/comments can legitimately contain words such as
    # "lock-free"; they are not allocation or synchronization operations.
    return re.sub(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"', "", source, flags=re.S)


class CensusStaticContract(unittest.TestCase):
    def test_no_heap_blocking_lock_io_or_runtime_dependencies(self):
        includes = re.findall(r"^#include\s+(.+)$", HEADER, flags=re.M)
        self.assertEqual(includes, ["<array>", "<atomic>", "<cstddef>", "<cstdint>", "<limits>", "<type_traits>"])
        code = code_only(HEADER)
        self.assertNotRegex(code, r"\b(new|malloc|calloc|realloc|free)\b")
        self.assertNotRegex(code, r"std::(vector|map|unordered_map|string|function|thread|mutex|recursive_mutex|lock_guard|unique_lock|condition_variable)\b")
        self.assertNotRegex(code, r"\b(fopen|printf|fprintf|Logger|CreateFile|WriteFile)\b")

    def test_api_names_exactly_match_enum(self):
        api = block(HEADER, "enum class Api", "inline constexpr std::size_t ApiCount")
        names = re.findall(r"\b[A-Za-z][A-Za-z0-9]*\b", api[api.index("{") + 1:api.index("}")])
        self.assertEqual(names[-1], "Count")
        self.assertEqual(len(names[:-1]), 48)
        self.assertEqual(len(set(names[:-1])), 48)
        labels = re.findall(r'"([A-Za-z0-9]+)"', block(HEADER, "ApiNames =", "inline constexpr const char* ApiName"))
        self.assertEqual(names[:-1], labels)
        for name in ["DrawPrimitive", "DrawIndexedPrimitive", "DrawPrimitiveUP", "DrawIndexedPrimitiveUP", "ProcessVertices"]:
            self.assertIn(name, names)

    def test_pure_snapshot_borrow_stays_private_to_production_wrapper(self):
        self.assertIn("const Snapshot& snapshot() const noexcept { return m_snapshot; }", HEADER)
        self.assertNotRegex(HEADER, r"(?<!const )Snapshot& snapshot\(\)")
        self.assertIn("Snapshot m_snapshot;", block(HEADER, "private:", "static_assert(sizeof(Census)"))
        wrapper = block(HEADER, "class ConcurrentCensus", "static_assert(std::atomic")
        public = wrapper.split("private:", 1)[0]
        self.assertIn("bool trySnapshot(CensusCopy& out) noexcept", public)
        self.assertNotRegex(public, r"(?:const )?(?:Snapshot|Census)&\s+\w+\(")
        self.assertNotIn("return m_census.snapshot()", wrapper)
        self.assertIn("Census m_census;", wrapper.split("private:", 1)[1])
        self.assertIn("static_assert(std::is_trivially_copyable<CensusCopy>::value);", HEADER)
        self.assertIn("static_assert(sizeof(CensusCopy) <= 8192", HEADER)
        self.assertIn("static_assert(sizeof(ConcurrentCensus) <= 8192", HEADER)

    def test_try_cell_has_one_acquire_no_retry_and_raii_release(self):
        wrapper = code_only(block(HEADER, "class ConcurrentCensus", "static_assert(std::atomic"))
        cell = block(wrapper, "class TryCell", "struct BusyCounter")
        self.assertEqual(cell.count(".test_and_set("), 1)
        self.assertIn("!cell.test_and_set(std::memory_order_acquire)", cell)
        self.assertRegex(cell, r"~TryCell\(\) noexcept\s*\{\s*if \(m_acquired\)\s*m_cell.clear\(std::memory_order_release\);")
        self.assertNotRegex(wrapper, r"\b(for|while|goto|compare_exchange_weak|compare_exchange_strong)\b")
        self.assertIn("TryCell(const TryCell&) = delete;", cell)
        self.assertIn("TryCell& operator=(const TryCell&) = delete;", cell)
        self.assertIn("std::atomic_flag m_cell = ATOMIC_FLAG_INIT;", wrapper)

    def test_grouped_transactions_share_one_refusal_boundary(self):
        for begin, end, api_call, fact_call in (
            ("bool noteDraw(", "bool noteTexture(", "m_census.noteCall(drawApi(facts.kind));", "m_census.noteDrawAttempt(facts);"),
            ("bool noteTexture(", "bool noteVsKey(", "m_census.noteCall(textureApi(facts.kind));", "m_census.noteTextureRequest(facts);"),
        ):
            with self.subTest(method=begin):
                method = code_only(block(HEADER, begin, end))
                self.assertEqual(method.count("TryCell cell(m_cell);"), 1)
                self.assertLess(method.index("return recordRefused();"), method.index(api_call))
                self.assertLess(method.index(api_call), method.index(fact_call))
                self.assertEqual(method.count("m_census.noteCall("), 1)
        ordinary = block(HEADER, "bool noteCall(Api api)", "bool noteDraw(")
        self.assertIn("m_census.noteCall(isGroupedApi(api) ? Api::Count : api);", ordinary)
        present = block(HEADER, "bool notePresent(", "bool trySnapshot(")
        self.assertIn("m_census.noteCall(Api::Present);", present)
        self.assertIn("m_census.snapshot().apiCalls[std::size_t(Api::Present)]", present)
        self.assertNotRegex(present, r"\+\+.*[Pp]resent|[Pp]resent.*\+\+")

    def test_sequence_and_copy_under_same_cell_failure_leaves_output_untouched(self):
        method = code_only(block(HEADER, "bool trySnapshot(", "ObservationRefusals observationRefusals()"))
        self.assertEqual(method.count("TryCell cell(m_cell);"), 1)
        self.assertLess(method.index("if (!cell.acquired())"), method.index("out.values = m_census.snapshot();"))
        self.assertLess(method.index("if (m_sequence == std::numeric_limits<uint64_t>::max())"), method.index("out.values = m_census.snapshot();"))
        self.assertIn("out.values.schemaVersion = 2;", method)
        self.assertIn("out.sequence = ++m_sequence;", method)
        self.assertEqual(method.count("return false;"), 2)

    def test_refusal_counter_bounded_atomic_cost_and_explicit_noncoherence(self):
        counter = code_only(block(HEADER, "struct BusyCounter", "bool recordRefused()"))
        self.assertEqual(counter.count(".fetch_add("), 1)
        self.assertIn("count.fetch_add(1, std::memory_order_relaxed)", counter)
        self.assertIn("overflow.store(1, std::memory_order_release)", counter)
        self.assertIn("std::atomic<uint32_t>::is_always_lock_free", HEADER)
        self.assertNotRegex(counter, r"\b(for|while|goto|compare_exchange_weak|compare_exchange_strong)\b")
        observation = block(HEADER, "ObservationRefusals observationRefusals()", "private:")
        self.assertNotIn("TryCell", observation)
        self.assertIn("recordOverflow ? std::numeric_limits<uint32_t>::max()", observation)
        self.assertIn("snapshotOverflow ? std::numeric_limits<uint32_t>::max()", observation)
        self.assertIn("Independent, non-coherent observations", HEADER)
        self.assertIn("Sticky boolean (0 or 1)", HEADER)

    def test_facts_do_not_carry_pointers(self):
        for start, end in [("struct DrawFacts", "enum class TextureKind"), ("struct TextureFacts", "inline constexpr std::size_t FactSampleCapacity")]:
            payload = code_only(block(HEADER, start, end))
            self.assertNotIn("*", payload)
            self.assertNotIn("&", payload)
        self.assertIn("enum class SourceAttribution : uint8_t { Unknown };", HEADER)

    def test_fixed_key_sizes_track_actual_local_types(self):
        vs = block(STATE, "struct D3D9FFShaderKeyVSData", "struct D3D9FFShaderKeyVS {")
        fs_stage = block(STATE, "struct D3D9FFShaderStage", "struct D3D9FFShaderKeyFS")
        vs_words = int(re.search(r"uint32_t Primitive\[(\d+)\]", vs).group(1))
        fs_stage_words = int(re.search(r"uint32_t Primitive\[(\d+)\]", fs_stage).group(1))
        self.assertRegex(CAPS, r"TextureStageCount\s*=\s*MaxSimultaneousTextures\s*;")
        stage_count = int(re.search(r"MaxSimultaneousTextures\s*=\s*(\d+)", CAPS).group(1))
        self.assertEqual(vs_words, 5)
        self.assertEqual(fs_stage_words * stage_count, 16)
        self.assertIn(f"VsKeyWords = {vs_words};", HEADER)
        self.assertIn(f"FsKeyWords = {fs_stage_words * stage_count};", HEADER)
        self.assertIn("KeySampleCapacity = 32;", HEADER)
        self.assertIn("NOT a portable ABI", HEADER)

    def test_missing_and_dropped_observations_explicit(self):
        for name in ["drawSamplesDropped", "textureSamplesDropped", "primitiveTypeMissing", "shaderPairMissing", "drawMissingFields", "textureMissingFields", "drawUnknownMaskBits", "textureUnknownMaskBits", "saturatedUpdates"]:
            self.assertIn(name, HEADER)
        self.assertIn("noteFixedFunctionVsKeyMissing", HEADER)
        self.assertIn("noteFixedFunctionFsKeyMissing", HEADER)
        self.assertIn("NOT exact unseen unique keys", HEADER)
        self.assertIn("0 means full mip chain", HEADER)

    def test_duplicates_are_checked_before_capacity_rejection(self):
        note_key = block(HEADER, "void noteKey(", "Snapshot m_snapshot;")
        self.assertLess(note_key.index("samples.keys[i] == key"), note_key.index("samples.storedUnique < Capacity"))
        self.assertIn("add(samples.dropped);", note_key)
        self.assertIn("add(samples.duplicates);", note_key)

    def test_saturation_uses_shared_production_helper(self):
        add = block(HEADER, "void add(uint64_t& value)", "static void maximize")
        self.assertIn("if (SaturatingAdd(value))", add)
        self.assertIn("SaturatingAdd(m_snapshot.saturatedUpdates);", add)
        self.assertIn("amount > max - value", HEADER)
        self.assertIn("testSaturatingArithmetic();", TEST)

    def test_cpu_companion_covers_positive_and_negative_boundaries(self):
        for name in ["testEmptyAndApiCalls", "testDrawKnownZeroAndMissing", "testDrawHistogramsAndInvalidFacts", "testTextureRequests", "testFactSampleCapacity", "testFixedFunctionKeys", "testSaturatingArithmetic", "testResetAndIndependentSnapshot", "testNoAllocations"]:
            self.assertIn(f"static void {name}()", TEST)
            self.assertIn(f"  {name}();", TEST)
        self.assertIn("CHECK(allocationCount == 1);", TEST)
        self.assertIn("CHECK(allocationCount == 0);", TEST)
        self.assertIn("#include \"../src/d3d9/d3d9_migration_census.h\"", TEST)
        self.assertNotIn("#define private", TEST)
        self.assertNotIn("const_cast", TEST)

    def test_no_gpu_or_success_claims_in_counter_names(self):
        snapshot = code_only(block(HEADER, "struct Snapshot", "inline bool SaturatingAdd"))
        self.assertIn("observedCalls", snapshot)
        self.assertIn("drawAttempts", snapshot)
        self.assertIn("observedTextureRequests", snapshot)
        self.assertNotRegex(snapshot, r"\b(successfulDraws|gpuDraws|gameDraws|allocatedTextures)\b")


if __name__ == "__main__":
    unittest.main(verbosity=2)
