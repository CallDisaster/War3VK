from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")

loop = DEVICE.split("for (size_t buildIndex = 0u;", 1)[1].split(
    "if (traceBuildEligible) {", 1
)[0]

trace = loop.index("const bool traceBuildRecord")
record_optional = loop.index(
    "std::optional<War3BuildEligibleRecordRawTiming> buildRecordTiming", trace
)
record_gate = loop.index("if (traceBuildRecord)", record_optional)
record_emplace = loop.index("buildRecordTiming.emplace(", record_gate)
record_enter = loop.index("enterBuildRecordPhase(", record_emplace)
assert trace < record_optional < record_gate < record_emplace < record_enter
assert "War3BuildEligibleRecordRawTiming buildRecordTiming(" not in loop

packet_optional = loop.index(
    "std::optional<War3PacketBuildRawTiming> packetBuildTiming", record_enter
)
packet_gate = loop.index("if (traceBuildRecord)", packet_optional)
packet_emplace = loop.index("packetBuildTiming.emplace(", packet_gate)
packet_pass = loop.index(
    "packetBuildTiming.has_value() ? &*packetBuildTiming : nullptr",
    packet_emplace,
)
packet_finish = loop.index("packetBuildTiming->finish()", packet_pass)
assert packet_optional < packet_gate < packet_emplace < packet_pass < packet_finish
assert "War3PacketBuildRawTiming packetBuildTiming(" not in loop

for phase in (
    "GateFilter",
    "Prebuild",
    "PacketBuild",
    "Eligibility",
    "Manifest",
    "Identity",
    "Append",
):
    assert f"enterBuildRecordPhase(War3BuildEligibleRecordPhase::{phase})" in loop

print("build-eligible timing lazy construction static checks passed")
