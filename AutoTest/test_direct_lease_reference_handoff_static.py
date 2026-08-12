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

assert (
    "static thread_local std::vector<const EligibleRecord*>\n"
    "      s_submittedPartPacketLeaseRecords;"
) in body
assert "submittedPartPacketLeaseRecords.push_back(&eligible);" in body
assert "submittedPartPacketLeaseRecords.push_back(eligible);" not in body
assert "War3RebindEligibleRecordPacket(submittedPartPacketLeaseRecords.back())" not in body

# Both layouts finish every possible vector mutation before the bookkeeping
# callback can execute. The callback definition is intentionally shared and
# appears earlier in source, so validate the two actual invocation regions.
note = body.index("auto noteSubmittedKeys =")
capture = body.index("submittedPartPacketLeaseRecords.push_back(&eligible);", note)
lease_update = body.index('enterDirectDetailPhase("SubmitLeaseUpdate")', capture)
mutation_tokens = (
    "eligibleRecords.push_back(",
    "eligibleRecords.insert(",
    "eligibleRecords.erase(",
    "eligibleRecords.clear(",
    "eligibleRecords.resize(",
    "eligibleRecords.reserve(",
    "eligibleRecords =",
)
ungrouped = body.index("if (!useObjectGrouped) {", capture)
grouped = body.index("} else {\n    enterDirectDetailPhase(\"SubmitGroupSort\")", ungrouped)
materialize = body.index("eligibleRecords = std::move(sorted);", grouped)
grouped_submit = body.index("if (useSubmitPermutationView) {", materialize)
for token in mutation_tokens:
    assert token not in body[ungrouped:grouped], ("ungrouped", token)
    assert token not in body[grouped_submit:lease_update], ("grouped", token)
assert materialize < grouped_submit < lease_update

# The pointer is consumed within the same call and the only deep copy now goes
# directly into the persistent lease, followed by the existing owned-pointer
# rebind.
lease = body[lease_update:]
assert "for (const EligibleRecord* eligiblePtr" in lease
assert "const EligibleRecord& eligible = *eligiblePtr;" in lease
assert "lease.packet = eligible.packet;" in lease
assert "War3RebindShadowPacketOwnedResourcePointers(lease.packet);" in lease
assert lease.index("lease.packet = eligible.packet;") < lease.index(
    "War3RebindShadowPacketOwnedResourcePointers(lease.packet);"
)

print("direct lease reference handoff static checks passed")
