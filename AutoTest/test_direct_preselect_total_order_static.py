#!/usr/bin/env python3
"""Static contract for allocation-free deterministic preselect ordering."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(
    encoding="utf-8", errors="strict"
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


begin = SOURCE.index("enterDirectDetailPhase(\"PreselectRecordSort\")")
end = SOURCE.index("enterDirectDetailPhase(\"PreselectGroupBuild\")", begin)
body = SOURCE[begin:end]

require("std::sort(" in body, "preselect must use the in-place sort")
require("std::stable_sort(" not in body, "stable merge sort must not remain")
required_keys = [
    "a.selectionKey != b.selectionKey",
    "aRecord.layerIndex != bRecord.layerIndex",
    "aRecord.payloadWord108 != bRecord.payloadWord108",
    "aRecord.renderablePart != bRecord.renderablePart",
    "return a.recordIndex < b.recordIndex;",
]
positions = [body.index(key) for key in required_keys]
require(positions == sorted(positions), "preselect key order changed")

scan_begin = SOURCE.index("for (uint32_t recordIndex = 0u;", begin - 8000)
scan_end = SOURCE.index("enterDirectDetailPhase(\"PreselectRecordSort\")", scan_begin)
scan = SOURCE[scan_begin:scan_end]
require(
    "preselectedRecords.push_back(" in scan and "{recordIndex," in scan,
    "recordIndex must remain the monotonically appended source ordinal",
)

print("direct preselect total-order static contract passed")
