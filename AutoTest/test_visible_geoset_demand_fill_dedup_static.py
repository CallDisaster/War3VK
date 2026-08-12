#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src/d3d9/war3/shadow/war3_shadow_runtime_contract.cpp"
text = SOURCE.read_text(encoding="utf-8")

publish = text.split(
    "bool TryPublishMissingVisibleUnitGeosetBinding(", 1
)[1].split("void DemandFillVisibleUnitGeosetBindings", 1)[0]
assert "noteRuntimeGeosetBinding" in publish
assert publish.count("BackfillVisibleUnitGeosetBindingFromCache") == 1
assert "if (BackfillVisibleUnitGeosetBindingFromCache" not in publish

demand = text.split(
    "void DemandFillVisibleUnitGeosetBindings", 1
)[1].split("ShadowRenderableRecord ConvertVisible", 1)[0]
assert "kMaxDemandFillPerCapture = 64u" in demand
assert "std::array<void*, kMaxDemandFillPerCapture>" in demand
assert "seenMissingCount" in demand
assert demand.index("capturedThisFrame >= kMaxDemandFillPerCapture") < demand.index(
    "std::find(seenMissingGeosetData.begin()"
)
assert "std::vector<uint8_t> readyBinding(recordCount" in demand
assert "readyBinding[recordIndex] = uint8_t(1u)" in demand
assert "if (readyBinding[i] == 0u)" in demand
assert "std::unordered_set<void*> seenMissingGeosetData" not in demand

print("visible geoset demand-fill dedup static checks passed")
