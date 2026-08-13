from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(
    encoding="utf-8", errors="replace"
)

start = SOURCE.index("enterDirectDetailPhase(\"SubmitAppend\")")
end = SOURCE.index("enterDirectDetailPhase(\"SubmitCompleteness\")", start)
body = SOURCE[start:end]

# Large object groups keep scalar scratch capacity, never packet/GPU data.
assert "static thread_local std::vector<uint64_t> s_coreGateHeap" in body
assert "auto& coreGateHeap = s_coreGateHeap" in body
assert "coreGateHeap.clear();" in body
assert "std::vector<uint64_t> coreGateHeap;" not in body

# An unchanged authoritative core must not be copied every frame. Both the
# legacy policy and the epoch planner retain their original timestamps and
# absence-reset behavior after the equality guard.
assert body.count("if (coreSet.committedPartKeys != partKeys)") == 2
assert body.count("coreSet.committedPartKeys = partKeys;") >= 3

# Shrink is stable and in place. It preserves the two-observation retirement
# rule while retaining committedPartKeys capacity for the next frame.
assert "const auto retiredBegin = std::remove_if(" in body
assert "if (streak < 2u)" in body
assert "return true;" in body
assert "coreSet.committedPartKeys.erase(" in body
assert "std::vector<uint64_t> shrunken" not in body
assert "semanticSceneShadowManifestRetiredAfterAuthoritativeAbsenceCount++" in body

print("direct core-set update scratch static checks passed")
