#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/d3d9/war3/render/war3_current_draw_contract.cpp").read_text(
    encoding="utf-8"
)
POLICY = (ROOT / "src/d3d9/war3/render/war3_classified_counter.h").read_text(
    encoding="utf-8"
)

assert "g_groupSlotDecodeAttemptCount.fetch_add(" in SOURCE
assert "g_groupSlotDecodeHitCount" not in SOURCE
for bucket in (
    "g_groupSlotDecodeMissDisabledStream",
    "g_groupSlotDecodeMissNoStream",
    "g_groupSlotDecodeMissUnreadableStream",
    "g_groupSlotDecodeMissGroupOutOfRange",
):
    assert f"{bucket}.fetch_add(" in SOURCE
assert "summary.groupSlotDecodeHitCount = DeriveClassifiedSuccessCount(" in SOURCE
assert "attemptCount >= totalFailures ? attemptCount - totalFailures : 0u" in POLICY
assert "failureCount > (std::numeric_limits<uint64_t>::max)() - totalFailures" in POLICY

print("current-draw group-slot classified counter static checks passed")
