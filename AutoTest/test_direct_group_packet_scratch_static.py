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

for name in (
    "s_submittedPartPacketLeaseRecords",
    "s_eligibleRecords",
    "s_recycledEligibleRecords",
    "s_shadowEligibleManifestRecords",
    "s_objectGroups",
    "s_originalIndices",
):
    assert f"static thread_local std::vector" in body

assert "auto& eligibleRecords = s_eligibleRecords;" in body
assert "auto& recycledEligibleRecords = s_recycledEligibleRecords;" in body
assert "auto& shadowEligibleManifestRecords = s_shadowEligibleManifestRecords;" in body
assert (
    "auto& submittedPartPacketLeaseRecords =\n"
    "      s_submittedPartPacketLeaseRecords;"
) in body
assert "auto& objectGroups = s_objectGroups;" in body
assert "submittedPartPacketLeaseRecords.clear();" in body
assert (
    "RecycleScratchElements(\n"
    "      eligibleRecords, recycledEligibleRecords);"
) in body
assert "shadowEligibleManifestRecords.clear();" in body
assert "objectGroups.clear();" in body
assert "static thread_local std::vector<uint64_t> s_currentPartKeys;" in body
assert "auto& currentPartKeys = s_currentPartKeys;" in body
assert "currentPartKeys.clear();" in body
assert "static thread_local std::vector<uint64_t> s_leaseKeys;" in body
assert "auto& leaseKeys = s_leaseKeys;" in body
assert "leaseKeys.clear();" in body
assert "auto& originalIndices = s_originalIndices;" in body
assert "originalIndices.clear();" in body
assert "originalIndices.resize(eligibleRecordCount);" in body

# Retaining allocator capacity must not retain logical packet contents or
# remove the existing budget-bound reserves and pointer rebinds after moves.
assert body.index("RecycleScratchElements(") < body.index(
    "eligibleRecords.reserve("
)
acquire = body.split("const auto acquireEligibleRecord", 1)[1].split(
    "const auto recycleRejectedEligibleRecord", 1
)[0]
assert "ResetShadowDrawPacketPreserveScratch" not in acquire
assert "ResetCurrentDrawAuthoritativeSamplePreserveScratch" not in acquire
assert body.index("shadowEligibleManifestRecords.clear();") < body.index(
    "shadowEligibleManifestRecords.reserve("
)
assert body.index("submittedPartPacketLeaseRecords.clear();") < body.index(
    "submittedPartPacketLeaseRecords.reserve(eligibleRecordCount);"
)
assert body.index("objectGroups.clear();") < body.index(
    "objectGroups.reserve(eligibleRecordCount);"
)
assert body.index("currentPartKeys.clear();") < body.index(
    "currentPartKeys.reserve("
)
assert body.index("leaseKeys.clear();") < body.index(
    "leaseKeys.reserve(directPartPacketLeases.size());"
)
assert body.index("originalIndices.clear();") < body.index(
    "originalIndices.resize(eligibleRecordCount);"
)
assert body.index("originalIndices.resize(eligibleRecordCount);") < body.index(
    "std::stable_sort(originalIndices.begin(), originalIndices.end(),"
)
assert "War3RebindEligibleRecordPacket(eligibleRecords.back());" in body
assert "War3RebindEligibleRecordPackets(eligibleRecords);" in body
assert "directRecordCap * 2u" in body

print("direct grouped packet scratch static checks passed")
