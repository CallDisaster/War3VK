from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")
CONTRACT = (ROOT / "src/d3d9/war3/render/war3_current_draw_contract.cpp").read_text(
    encoding="utf-8"
)
RESOURCE = (ROOT / "src/d3d9/war3/shadow/war3_shadow_renderer_core.h").read_text(
    encoding="utf-8"
)
PROOF = (ROOT / "src/d3d9/war3/render/war3_immutable_group_slot_binding.h").read_text(
    encoding="utf-8"
)

assert "bool* outBorrowedImmutableHint" in CONTRACT
immutable_branch = CONTRACT[CONTRACT.index("if (immutableHint != nullptr") :]
immutable_branch = immutable_branch[: immutable_branch.index("outGroupSlots.resize")]
assert "*outBorrowedImmutableHint = true" in immutable_branch
assert "else\n      outGroupSlots.assign" in immutable_branch
assert "currentDrawGroupSlotsBackedByResourceKeepAlive" in RESOURCE
assert "ValidateImmutableGroupSlotBinding" in DEVICE
assert "owner->readyForShadowConsumer()" in DEVICE
assert "resource.resourceKeepAlive.get()" in DEVICE
assert "resource.currentDrawGroupSlotsBackedByResourceKeepAlive = true" in DEVICE
assert "if (!War3PacketResourceHasImmutableCurrentDrawGroupSlots(resource))" in DEVICE
assert "packet.resource.ownedVertexGroupIndices.size() >= size_t(vertexCount) ||" in DEVICE
assert "packet.resource.vertexGroupIndices" in DEVICE

for required in (
    "proof.packetMapEpoch == proof.ownerMapEpoch",
    "proof.packetImmutableGeneration == proof.ownerImmutableGeneration",
    "proof.packetSlots == proof.ownerSlots",
    "proof.ownerSlotCount >= proof.requiredVertexCount",
):
    assert required in PROOF

print("direct immutable group-slot binding static contract: ok")
