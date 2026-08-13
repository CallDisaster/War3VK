#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/d3d9/war3/render/war3_current_draw_contract.cpp").read_text(
    encoding="utf-8"
)

assert "g_publishAttemptCount.fetch_add(" in SOURCE
assert "g_publishReadyCount" not in SOURCE
for bucket in (
    "g_publishMissNoRenderablePart",
    "g_publishMissInvalidPaletteSlot",
    "g_publishMissInvalidPaletteCount",
    "g_publishMissNoGlobalPalette",
):
    assert f"{bucket}.fetch_add(" in SOURCE

summary = SOURCE[SOURCE.index("QueryCurrentDrawContractDiagnosticsSummary()") :]
assert "summary.publishReadyCount = DeriveClassifiedSuccessCount(" in summary
assert "publishRejectedNoTrusted" in summary
assert "summary.publishMissNoMeshPayload" not in summary[
    summary.index("summary.publishReadyCount =") :
    summary.index("summary.publishSkippedNonWorldContext")
]

print("current-draw publish ready classified counter static checks passed")
