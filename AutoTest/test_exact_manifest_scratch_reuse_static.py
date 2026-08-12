from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")

start = SOURCE.index("uint32_t D3D9DeviceEx::War3TryPopulateSemanticShadowScene")
end = SOURCE.index("bool D3D9DeviceEx::War3ExecuteSemanticShadowSceneForValidation", start)
body = SOURCE[start:end]

declaration = body.index("s_exactSubmittedManifestRecords;")
initial_clear = body.index("exactSubmittedManifestRecords.clear();", declaration)
guard = body.index("struct ExactSubmittedManifestScratchReset", initial_clear)
guard_clear = body.index("records.clear();", guard)
producer = body.index("War3TryPopulateDrawTimeSemanticProducer(", guard_clear)
grouped = body.index("War3TryPopulateDirectCurrentDrawGrouped(", producer)

assert "static thread_local std::vector<" in body[declaration - 120 : declaration]
assert declaration < initial_clear < guard < guard_clear < producer < grouped
assert "CurrentDrawContractRecord>& records" in body[guard:producer]
assert "~ExactSubmittedManifestScratchReset()" in body[guard:producer]
assert "exactSubmittedManifestScratchReset { exactSubmittedManifestRecords }" in body[guard:producer]
assert body.count("s_exactSubmittedManifestRecords") == 2

# The reused allocation is only the caller-side scalar manifest range. The
# Stage11 producer still rebuilds every logical record from current cache
# entries and clears the output before inspecting this frame.
producer_start = SOURCE.index("uint32_t D3D9DeviceEx::War3TryPopulateDrawTimeSemanticProducer")
producer_end = SOURCE.index("void D3D9DeviceEx::War3CollectRetiredShadowSessions", producer_start)
producer_body = SOURCE[producer_start:producer_end]
assert "exactSubmittedManifestRecords.clear();" in producer_body
assert "CurrentDrawContractRecord exactRecord = {};" in producer_body
assert "exactSubmittedManifestRecords.push_back(std::move(exactRecord));" in producer_body

print("exact manifest scratch reuse static checks passed")
