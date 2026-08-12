"""Lock immutable DirectGrouped index-slice ownership and validation."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")
HEADER = (
    ROOT / "src/d3d9/war3/shadow/war3_shadow_renderer_core.h"
).read_text(encoding="utf-8")


class DirectImmutableIndexSliceOwnerStaticTests(unittest.TestCase):
    def test_provenance_is_explicit_and_default_false(self) -> None:
        self.assertIn(
            "bool dynamicIndexBackedByResourceKeepAlive = false", HEADER
        )
        self.assertIn(
            "resource.dynamicIndexBackedByResourceKeepAlive = true", DEVICE
        )

    def test_validator_requires_exact_owner_generation_and_range(self) -> None:
        start = DEVICE.index(
            "bool War3PacketResourceHasImmutableDynamicIndexSlice("
        )
        end = DEVICE.index(
            "bool D3D9DeviceEx::War3TryAppendSemanticShadowPacket(", start
        )
        validator = DEVICE[start:end]
        for token in (
            "dynamicIndexBackedByResourceKeepAlive",
            "resource.resourceKeepAlive == nullptr",
            "owner->readyForShadowConsumer()",
            "owner->mapEpoch != resource.mapEpoch",
            "owner->immutableModelGeneration != resource.immutableModelGeneration",
            "owner->geosetIndex != resource.geosetIndex",
            "owner->modelKey != resource.modelKey",
            "owner->contentHash != resource.contentHash",
            "indexCount > owner->indices.size() - baseIndex",
            "resource.dynamicIndexStream == owner->indices.data() + baseIndex",
        ):
            self.assertIn(token, validator)

    def test_consumer_and_lease_both_use_the_same_validator(self) -> None:
        append_start = DEVICE.index(
            "bool D3D9DeviceEx::War3TryAppendSemanticShadowPacket(\n"
            "    const dxvk::war3::shadow::ShadowDrawPacket& packet,\n"
            "    const dxvk::war3::render::CurrentDrawAuthoritativeSample*\n"
            "        directCurrentDrawSample,\n"
            "    bool fromStalePoseRestore,\n"
            "    bool currentFrameExactOwnerPrefiltered)"
        )
        append_end = DEVICE.index(
            "uint32_t D3D9DeviceEx::War3TryPopulateDirectCurrentDrawGrouped",
            append_start,
        )
        append = DEVICE[append_start:append_end]
        self.assertIn(
            "War3PacketResourceHasImmutableDynamicIndexSlice(packet.resource)",
            append,
        )
        self.assertIn("!hasImmutableDynamicIndexStream", append)

        lease_start = DEVICE.index("auto packetSafeForDirectPartLease")
        lease_end = DEVICE.index("static thread_local std::vector", lease_start)
        lease = DEVICE[lease_start:lease_end]
        self.assertIn(
            "!War3PacketResourceHasImmutableDynamicIndexSlice(packet.resource)",
            lease,
        )
        restore_start = DEVICE.index("if (!leaseInfo.sliceFresh")
        restore_end = DEVICE.index(
            "DirectObjectCompletenessBucket* bucket", restore_start
        )
        self.assertIn(
            "!War3PacketResourceHasImmutableDynamicIndexSlice(\n"
            "              leased.packet.resource)",
            DEVICE[restore_start:restore_end],
        )

    def test_submit_verifier_includes_provenance(self) -> None:
        self.assertIn(
            "resource.dynamicIndexBackedByResourceKeepAlive", DEVICE
        )
        self.assertIn(
            "ag.dynamicIndexBackedByResourceKeepAlive ==\n"
            "                  bg.dynamicIndexBackedByResourceKeepAlive",
            DEVICE,
        )


if __name__ == "__main__":
    unittest.main()
