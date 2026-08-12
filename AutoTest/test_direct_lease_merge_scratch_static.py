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
lease = body.split('enterBuildEligiblePhase("LeaseRestore");', 1)[1].split(
    'enterBuildEligiblePhase("StickyFilter");', 1
)[0]

assert "static thread_local std::vector<EligibleRecord> s_restoredLeaseRecords;" in lease
assert "auto& restoredLeaseRecords = s_restoredLeaseRecords;" in lease
first_clear = lease.index("restoredLeaseRecords.clear();")
reserve = lease.index("restoredLeaseRecords.reserve(leaseKeys.size());")
append = lease.index("restoredLeaseRecords.push_back(std::move(leased));")
merge = lease.index('enterBuildEligibleLeasePhase("LeaseMerge");')
insert = lease.index("eligibleRecords.insert(", merge)
second_clear = lease.index("restoredLeaseRecords.clear();", insert)
rebind = lease.index("War3RebindEligibleRecordPackets(eligibleRecords);", second_clear)
assert first_clear < reserve < append < merge < insert < second_clear < rebind

# Preserve restored-before-live order without allocating a third packet vector.
insert_body = lease[insert:second_clear]
assert "eligibleRecords.begin()," in insert_body
assert "std::make_move_iterator(restoredLeaseRecords.begin())" in insert_body
assert "std::make_move_iterator(restoredLeaseRecords.end())" in insert_body
assert "std::vector<EligibleRecord> mergedRecords" not in lease
assert "eligibleRecords = std::move(mergedRecords)" not in lease

print("direct lease merge scratch static checks passed")
