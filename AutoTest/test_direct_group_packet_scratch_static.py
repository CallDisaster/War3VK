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
    "s_objectGroups",
):
    assert f"static thread_local std::vector" in body

assert "auto& eligibleRecords = s_eligibleRecords;" in body
assert (
    "auto& submittedPartPacketLeaseRecords =\n"
    "      s_submittedPartPacketLeaseRecords;"
) in body
assert "auto& objectGroups = s_objectGroups;" in body
assert "submittedPartPacketLeaseRecords.clear();" in body
assert "eligibleRecords.clear();" in body
assert "objectGroups.clear();" in body

# Retaining allocator capacity must not retain logical packet contents or
# remove the existing budget-bound reserves and pointer rebinds after moves.
assert body.index("eligibleRecords.clear();") < body.index(
    "eligibleRecords.reserve("
)
assert body.index("submittedPartPacketLeaseRecords.clear();") < body.index(
    "submittedPartPacketLeaseRecords.reserve(eligibleRecordCount);"
)
assert body.index("objectGroups.clear();") < body.index(
    "objectGroups.reserve(eligibleRecordCount);"
)
assert "War3RebindEligibleRecordPacket(eligibleRecords.back());" in body
assert "War3RebindEligibleRecordPackets(eligibleRecords);" in body
assert "directRecordCap * 2u" in body

print("direct grouped packet scratch static checks passed")
