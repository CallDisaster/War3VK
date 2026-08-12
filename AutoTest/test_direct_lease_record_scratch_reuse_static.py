from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(
    encoding="utf-8", errors="replace"
)

start = SOURCE.index("for (size_t leaseIndex")
end = SOURCE.index("if (traceBuildEligible)", start)
scan = SOURCE[start:end]

copy = scan.index("EligibleRecord leased = acquireEligibleRecord();")
packet_copy = scan.index("leased.packet = leaseIt->second.packet;", copy)
assert copy < packet_copy
assert "EligibleRecord leased = {};" not in scan

# All post-copy reject paths return owned vector capacity to the same-call
# scratch pool. No eligibility, pose, backing, or slice condition is removed.
pose_reject = scan.index("semanticSceneShadowManifestPartLeaseRejectedPoseStaleCount++")
backing_reject = scan.index("if (!packetSafeForDirectPartLease(leased, false))")
slice_reject = scan.index("semanticSceneShadowManifestPartLeaseRejectedSliceStaleCount++")
for reject in (pose_reject, backing_reject, slice_reject):
    tail = scan[reject : reject + 320]
    assert "recycleRejectedEligibleRecord(std::move(leased));" in tail
    assert "continue;" in tail

append = scan.index("restoredLeaseRecords.push_back(std::move(leased))")
assert slice_reject < append

print("direct lease record scratch reuse static checks passed")
