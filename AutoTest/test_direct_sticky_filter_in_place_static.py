#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEVICE = ROOT / "src/d3d9/d3d9_device.cpp"

source = DEVICE.read_text(encoding="utf-8")
body = source.split(
    "uint32_t D3D9DeviceEx::War3TryPopulateDirectCurrentDrawGrouped", 1
)[1].split(
    "bool D3D9DeviceEx::War3TryPopulateSemanticShadowScene", 1
)[0]
sticky = body.split('enterBuildEligiblePhase("StickyFilter");', 1)[1].split(
    "// --- Step 3: submit ---", 1
)[0]

# The threshold decision must still happen before any packet move. Once the
# sticky cohort is accepted, compact the existing vector stably and rebind all
# packet-owned pointer aliases before submission.
threshold = sticky.index("if (retainedRecordCount >= minStickyPartRecords)")
remove = sticky.index("std::remove_if(", threshold)
rebind = sticky.index("War3RebindEligibleRecordPackets(eligibleRecords);", remove)
submit_boundary = sticky.index(
    "eligibleRecordCount = uint32_t(eligibleRecords.size());", rebind
)
assert threshold < remove < rebind < submit_boundary
assert "return !eligible.previouslySubmittedPart;" in sticky[remove:rebind]

# No secondary packet vector or move-assignment may reintroduce allocation and
# ownership churn in the accepted sticky path.
assert "std::vector<EligibleRecord> retainedRecords" not in sticky
assert "eligibleRecords = std::move(retainedRecords)" not in sticky

# Counters retain their prior meanings even though size changes in place.
assert "const uint32_t droppedRecordCount" in sticky
assert "semanticSceneDirectStickyPartSelectionRetainedCount" in sticky
assert "semanticSceneDirectStickyPartSelectionDroppedCount" in sticky
assert "droppedRecordCount;" in sticky
assert "semanticSceneDirectStickyPartSelectionFallbackCount" in sticky

print("direct sticky filter in-place static checks passed")
