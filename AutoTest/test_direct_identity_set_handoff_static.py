from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")


populate = DEVICE.split(
    "uint32_t D3D9DeviceEx::War3TryPopulateDirectCurrentDrawGrouped(", 1
)[1].split("uint32_t D3D9DeviceEx::War3TryPopulateSemanticShadowScene(", 1)[0]

# Previous submitted sets are immutable references. The normal path performs a
# single strict-order check and sorts only if broad lease insertion (or a
# future invariant regression) introduced disorder/duplicates.
assert "const std::vector<uint64_t>& previousSubmittedSelectionKeys" in populate
assert "preferredSelectionKeysAlreadySortedUnique" in populate
assert "std::adjacent_find(" in populate
assert "return a >= b;" in populate
normalize = populate.index("if (!preferredSelectionKeysAlreadySortedUnique)")
sort = populate.index("std::sort(preferredSelectionKeys.begin()", normalize)
unique = populate.index("std::unique(preferredSelectionKeys.begin()", sort)
binary = populate.index("std::binary_search(preferredSelectionKeys.begin()", unique)
assert normalize < sort < unique < binary

# Submitted sets are normalized once. Jaccard consumes those sorted references
# directly and must not take vectors by value or sort inside the metric.
diag = populate.split("// identity hash:", 1)[1]
assert "computeSortedJaccardMilli" in diag
jaccard = diag.split("const auto computeSortedJaccardMilli", 1)[1].split(
    "const auto sortUniqueKeys", 1
)[0]
assert "const std::vector<uint64_t>& previousKeys" in jaccard
assert "const std::vector<uint64_t>& currentKeys" in jaccard
assert "std::sort(" not in jaccard
assert "std::unique(" not in jaccard
assert diag.index("sortUniqueKeys(submittedIdentityKeys)") < diag.index(
    "computeSortedJaccardMilli(previousSubmittedObjectIdentityKeys"
)

# Common publication swaps backing allocations with TLS scratch so neither
# side reallocates every frame. Part identity keeps the historical fallback to
# object identities when no exact part key was published.
for target, source in (
    ("m_war3SemanticDirectPrevSubmittedIdentityKeys", "submittedPreferenceKeys"),
    ("m_war3SemanticDirectPrevSubmittedObjectIdentityKeys", "submittedIdentityKeys"),
    ("m_war3SemanticDirectPrevSubmittedPartIdentityKeys", "submittedPartIdentityKeys"),
):
    assert re.search(rf"{target}\.swap\(\s*{source}\)", diag)
assert "if (!submittedPartIdentityKeys.empty())" in diag
assert (
    "m_war3SemanticDirectPrevSubmittedPartIdentityKeys =\n"
    "          m_war3SemanticDirectPrevSubmittedObjectIdentityKeys;"
) in diag
assert "std::vector<uint64_t> sortedObjectKeys" not in diag
assert "std::vector<uint64_t> sortedPreferenceKeys" not in diag
assert "std::vector<uint64_t> sortedKeys" not in diag

print("direct identity set handoff static checks passed")
