#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/d3d9/war3/render/war3_current_draw_contract.cpp").read_text(
    encoding="utf-8"
)

assert "g_queryHitCount" not in SOURCE
assert "g_capturedPaletteQueryHitCount" not in SOURCE
assert "summary.queryHitCount = DeriveClassifiedSuccessCount(" in SOURCE
assert "summary.capturedPaletteQueryHitCount = DeriveClassifiedSuccessCount(" in SOURCE

for bucket in (
    "g_queryMissNoRecord",
    "g_queryMissFrameTagMismatch",
    "g_queryMissCacheCollision",
    "g_capturedPaletteMissNoContract",
    "g_capturedPaletteMissInvalidCount",
    "g_capturedPaletteMissNoSnapshot",
):
    assert f"{bucket}.fetch_add(" in SOURCE

query_start = SOURCE.index("bool QueryCurrentDrawContract(")
query_end = SOURCE.index("bool QueryCurrentDrawGeometryContract(", query_start)
query = SOURCE[query_start:query_end]
assert "g_queryAttemptCount.fetch_add(" in query
assert query.count("return false;") == 1
assert query.count("return true;") == 1

palette_start = SOURCE.index("bool DecodeCapturedPaletteForRecord(")
palette_end = SOURCE.index("\n} // namespace", palette_start)
palette = SOURCE[palette_start:palette_end]
assert "g_capturedPaletteQueryAttemptCount.fetch_add(" in palette
assert palette.count("return true;") == 3
assert palette.count("g_capturedPaletteMiss") >= 3

print("current-draw query classified hit counter static checks passed")
