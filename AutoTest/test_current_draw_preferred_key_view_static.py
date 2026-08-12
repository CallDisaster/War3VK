#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src/d3d9/war3/render/war3_current_draw_contract.h"
CONTRACT = ROOT / "src/d3d9/war3/render/war3_current_draw_contract.cpp"
DEVICE = ROOT / "src/d3d9/d3d9_device.cpp"

header = HEADER.read_text(encoding="utf-8")
contract = CONTRACT.read_text(encoding="utf-8")
device = DEVICE.read_text(encoding="utf-8")

options = header.split("struct CurrentDrawContractSnapshotOptions", 1)[1].split(
    "};", 1
)[0]
assert "std::vector<uint64_t> preferredSelectionKeys;" in options
assert (
    "const std::vector<uint64_t>* preferredSelectionKeysView = nullptr;"
    in options
)
assert "bool preferredSelectionKeysViewSortedUnique = false;" in options

snapshot = contract.split(
    "void SnapshotPublishedCurrentDrawContracts(\n"
    "    const CurrentDrawContractSnapshotOptions& options,", 1
)[1].split("enterSnapshotPhase(\"SnapshotScratchSetup\")", 1)[0]
assert "options.preferredSelectionKeysView != nullptr" in snapshot
assert "? *options.preferredSelectionKeysView" in snapshot
assert ": options.preferredSelectionKeys;" in snapshot
assert "std::is_sorted(preferredSelectionKeys.begin()" in snapshot
assert "options.preferredSelectionKeysViewSortedUnique" in snapshot
assert "std::binary_search(preferredSelectionKeys.begin()" in snapshot
assert "options.preferredSelectionKeys.begin()" not in snapshot

populate = device.split(
    "uint32_t D3D9DeviceEx::War3TryPopulateDirectCurrentDrawGrouped", 1
)[1]
assert "static thread_local std::vector<uint64_t> s_leasedSelectionKeys;" in populate
assert "static thread_local std::vector<uint64_t> s_preferredSelectionKeys;" in populate
assert "leasedSelectionKeys.clear();" in populate
assert "preferredSelectionKeys.assign(previousSubmittedSelectionKeys.begin()," in populate
assert "snapshotOptions.preferredSelectionKeysView = &preferredSelectionKeys;" in populate
assert "snapshotOptions.preferredSelectionKeysViewSortedUnique = true;" in populate
assert "snapshotOptions.preferredSelectionKeys = preferredSelectionKeys;" not in populate

print("current-draw preferred key view static checks passed")
