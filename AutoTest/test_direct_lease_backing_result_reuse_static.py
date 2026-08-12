from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(
    encoding="utf-8", errors="replace"
)

start = SOURCE.index("auto packetSafeForDirectPartLease")
end = SOURCE.index("// Both lists are bounded", start)
gate = SOURCE[start:end]

assert "EligibleRecord& eligible" in gate
query = gate.index("War3SemanticDirectPacketHasMainWorldVisibleBacking(")
store = gate.index("eligible.mainWorldBackingStatus = backingStatus", query)
decision = gate.index("if (!hasMainWorldVisibleBacking)", store)
assert query < store < decision

note_start = SOURCE.index("auto noteSubmittedKeys")
note_end = SOURCE.index("// The exact producer already published", note_start)
note = SOURCE[note_start:note_end]
assert "EligibleRecord& eligible" in note
assert "packetSafeForDirectPartLease(eligible, true)" in note

# The restore safety gate writes a current-call value result. Submission may
# reuse it, but a never-checked packet still falls through the canonical
# registry query inside packetSafeForDirectPartLease.
assert "backingStatus != War3SemanticDirectMainWorldBackingStatus::NotChecked" in gate

print("direct lease backing-result reuse static checks passed")
