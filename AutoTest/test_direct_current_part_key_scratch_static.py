from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(
    encoding="utf-8", errors="replace"
)

start = SOURCE.index("enterBuildEligibleLeasePhase(\"LeaseCurrentKeys\")")
end = SOURCE.index("enterBuildEligibleLeasePhase(\"LeaseKeySort\")", start)
build = SOURCE[start:end]

scan_start = SOURCE.index("for (size_t leaseIndex", end)
scan_end = SOURCE.index("if (traceBuildEligible)", scan_start)
scan = SOURCE[scan_start:scan_end]

assert "static thread_local std::vector<uint64_t> s_currentPartKeys" in build
assert "std::unordered_set<uint64_t> s_currentPartKeys" not in build
assert "currentPartKeys.push_back(eligible.manifestPartLeaseKey)" in build
assert "currentPartKeys.push_back(partKey)" in build
assert "std::sort(currentPartKeys.begin(), currentPartKeys.end())" in build
assert "std::unique(currentPartKeys.begin(), currentPartKeys.end())" in build
assert "std::binary_search(currentPartKeys.begin(), currentPartKeys.end()," in scan

# leaseKeys contains each persistent map key exactly once. Once the loop has
# reached a key, inserting that same key into the current set cannot affect a
# later membership decision and only charged a node/hash mutation.
assert "currentPartKeys.insert(key)" not in scan

print("direct current part-key scratch static checks passed")
