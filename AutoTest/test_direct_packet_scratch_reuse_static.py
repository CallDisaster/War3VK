#!/usr/bin/env python3
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")
CONTRACT = (ROOT / "src/d3d9/war3/render/war3_current_draw_contract.cpp").read_text(encoding="utf-8")
HEADER = (ROOT / "src/d3d9/war3/render/war3_direct_packet_scratch.h").read_text(encoding="utf-8")


def body_after(text: str, needle: str, span: int) -> str:
    start = text.index(needle)
    return text[start:start + span]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


builder = body_after(
    DEVICE, "bool War3TryBuildShadowPacketFromCurrentDrawRecord(", 38000)
populate = body_after(DEVICE, "struct EligibleRecord {", 125000)
record_resolver = body_after(
    CONTRACT, "ResolveCurrentDrawAuthoritativeSampleFromRecord(", 3000)

require("ResetShadowDrawPacketPreserveScratch(out);" in builder,
        "packet builder does not clear recycled authority before use")
require(builder.index("ResetShadowDrawPacketPreserveScratch(out);") <
        builder.index("if (record.renderablePart == nullptr"),
        "recycled packet reset occurs after current-frame gates")
require("directCurrentDrawSample.palette = std::move(out.runtimeGroupPalette)" in builder and
        "std::move(out.resource.ownedVertexGroupIndices)" in builder,
        "hot palette/group allocations are not carried into current decode")
require("directCurrentDrawSample.palette.swap(outDirectCurrentDrawSample->palette)" in builder and
        "directCurrentDrawSample.groupSlots.swap(" in builder,
        "failed decode sample capacity is not returned to packet scratch")
require("liveRebuiltPalette = std::move(directCurrentDrawSample.palette);" in builder,
        "live fallback allocates a second palette instead of reusing scratch")
require("out.runtimeGroupPalette = std::move(liveRebuiltPalette);" in builder and
        "std::move(directCurrentDrawSample.groupSlots)" in builder,
        "failed live fallback discards warmed packet scratch")
require("directCurrentDrawSample.groupSlots.clear();" in builder and
        "resource.ownedVertexGroupIndices =" in builder,
        "unused decode group capacity is not returned to packet scratch")

require("s_recycledEligibleRecords" in populate and
        "RecycleScratchElements(" in populate and
        "AcquireScratchElement(" in populate,
        "eligible records do not use the bounded per-thread recycler")
require("eligibleRecords.clear();" not in populate[:4000],
        "eligible records are still destroyed at frame start")
require(populate.count("recycleRejectedEligibleRecord(std::move(eligible))") >= 3,
        "known post-build rejection paths discard their scratch")

require("packet = {};" in HEADER,
        "scratch reset does not clear packet scalar/owner state")
for stale in ("resourceKeepAlive", "ownedDynamicIndices",
              "dynamicIndexStream", "renderable"):
    require(stale not in re.sub(r"packet = \{\};[\s\S]*", "", HEADER),
            f"unexpected special-case authority preservation for {stale}")
require("ResetCurrentDrawAuthoritativeSamplePreserveScratch(out);" in record_resolver,
        "record resolver destroys or retains authoritative sample incorrectly")

print("test_direct_packet_scratch_reuse_static: 12/12 passed")
