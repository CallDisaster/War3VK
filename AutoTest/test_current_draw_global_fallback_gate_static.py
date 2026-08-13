#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/d3d9/war3/render/war3_current_draw_contract.cpp").read_text(
    encoding="utf-8"
)


def body(signature: str, following: str) -> str:
    start = SOURCE.index(signature)
    end = SOURCE.index(following, start)
    return SOURCE[start:end]


policy = body("bool GlobalCurrentDrawPublishEnabled()", "bool CurrentDrawActiveSlotSnapshotEnabled()")
assert 'ReadEnvU32("DXVK_WAR3_CURRENT_DRAW_GLOBAL_PUBLISH", 0u)' in policy

lookup = body("ContractLookupStatus LookupCurrentDrawContractRecord(", "uint32_t SnapshotPriorityOf(")
gate = lookup.index("if (!GlobalCurrentDrawPublishEnabled())")
lock = lookup.index("std::lock_guard<std::mutex> lock(g_publishedCurrentDrawMutex)")
assert gate < lock
assert "ContractLookupStatus::CacheCollision" in lookup[gate:lock]
assert "ContractLookupStatus::MissingRecord" in lookup[gate:lock]

palette = body("bool DecodeCapturedPaletteForRecord(", "\n} // namespace")
local = palette.index("FindLocalPaletteSnapshot")
global_gate = palette.index("if (GlobalCurrentDrawPublishEnabled())", local)
global_lock = palette.index("std::lock_guard<std::mutex> lock(g_publishedCurrentDrawMutex)", local)
assert local < global_gate < global_lock
assert "g_publishedPaletteSnapshotByPart.find" in palette[global_lock:]
assert "g_publishedPaletteSnapshotByAttribution.find" in palette[global_lock:]
assert "g_capturedPaletteMissNoSnapshot.fetch_add(" in palette[global_lock:]

print("current-draw global fallback gate static checks passed")
