from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/d3d9/war3/render/war3_current_draw_contract.cpp").read_text(
    encoding="utf-8"
)


snapshot = SOURCE.split(
    "void SnapshotPublishedCurrentDrawContracts(\n"
    "    const CurrentDrawContractSnapshotOptions& options,\n"
    "    std::vector<CurrentDrawContractRecord>& out) {",
    1,
)[1].split("CurrentDrawResolveStatus ResolveCurrentDrawAuthoritativeSample", 1)[0]

assert "kCurrentDrawSnapshotDedupeIndexSize =" in SOURCE
assert "kContractCacheSize * 2u" in SOURCE
assert "std::array<CurrentDrawSnapshotDedupeIndexEntry," in snapshot
assert "s_snapshotDedupeIndexGeneration" in snapshot
assert "if (snapshotDedupeIndexGeneration == 0u)" in snapshot
assert "s_snapshotDedupeIndex = {};" in snapshot

# Hash selects a probe origin only. Equality must cover the complete part slice.
key = SOURCE.split("struct CurrentDrawSnapshotPartKey {", 1)[1].split(
    "struct CurrentDrawSnapshotPartKeyHash", 1
)[0]
for field in ("renderablePart", "layerIndex", "payloadWord108", "payloadWord11C"):
    assert f"{field} == other.{field}" in key
assert "entry.key == key" in snapshot
assert "probe < kCurrentDrawSnapshotDedupeIndexSize" in snapshot

# Unlimited and batched paths must use the same exact index. Saturation is
# fail-safe: a miss scans the already accepted records instead of appending an
# unverified duplicate or treating a hash collision as identity.
assert snapshot.count("findSnapshotDedupeIndex(dedupeKey)") == 2
assert snapshot.count("storeSnapshotDedupeIndex(dedupeKey, out.size())") == 2
assert snapshot.count("findSnapshotDedupeLinear(dedupeKey)") == 2
assert "snapshotDedupeIndexOverflowed = true" in snapshot
assert "std::numeric_limits<size_t>::max()" in snapshot

# The node maps and folded-hash identity used by the two fast modes are gone.
assert "s_unlimitedDedupeIndex" not in snapshot
assert "s_batchedBoundedDedupeIndex" not in snapshot
assert "unlimitedDedupeKey" not in snapshot
assert "unordered_map<\n      CurrentDrawSnapshotPartKey" not in snapshot

# Historical online bounded top-K still keeps its exact linear part-slice
# comparison and ordering semantics.
bounded = snapshot.split("if (options.maxRecords == 0u) {", 1)[1].split(
    "enterSnapshotPhase(\"SnapshotActiveSlotScan\")", 1
)[0]
assert "existing.renderablePart == record.renderablePart" in bounded
assert "existing.layerIndex == record.layerIndex" in bounded
assert "existing.payloadWord108 == record.payloadWord108" in bounded
assert "existing.payloadWord11C == record.payloadWord11C" in bounded

print("current-draw exact snapshot dedupe index static checks passed")
