from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")

populate = DEVICE.split(
    "uint32_t D3D9DeviceEx::War3TryPopulateDirectCurrentDrawGrouped(", 1
)[1].split("uint32_t D3D9DeviceEx::War3TryPopulateSemanticShadowScene(", 1)[0]

acquire = populate.split("const auto acquireEligibleRecord", 1)[1].split(
    "const auto recycleRejectedEligibleRecord", 1
)[0]
assert "ResetShadowDrawPacketPreserveScratch" not in acquire
assert "ResetCurrentDrawAuthoritativeSamplePreserveScratch" not in acquire

builder = DEVICE.split(
    "bool War3TryBuildShadowPacketFromCurrentDrawRecord(", 1
)[1].split(
    "bool War3LooksSubmitEligibleForDirectCurrentDrawFast(", 1
)[0]
packet_reset = builder.index("ResetShadowDrawPacketPreserveScratch(out)")
sample_reset = builder.index(
    "ResetCurrentDrawAuthoritativeSamplePreserveScratch(", packet_reset
)
identity_gate = builder.index("record.renderablePart == nullptr", sample_reset)
assert packet_reset < sample_reset < identity_gate

prebuild = populate.split("auto tryBuildDrawTimePrebuildBypassEligible", 1)[1].split(
    "War3PopulateReadableRegionCache currentDrawGroupRangeCache", 1
)[0]
assert "eligible.packet = std::move(packet)" in prebuild
assert "eligible.sample = {}" in prebuild
assert "eligible.fromDrawTimePrebuildBypass = true" in prebuild

print("direct packet single-reset static checks passed")
