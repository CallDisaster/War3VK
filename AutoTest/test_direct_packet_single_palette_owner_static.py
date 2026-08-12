from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")


class DirectPacketSinglePaletteOwnerStaticTests(unittest.TestCase):
    def test_ready_packet_moves_payload_without_sample_vector_copy(self) -> None:
        start = DEVICE.index(
            "if (directResolveStatus ==\n"
            "            dxvk::war3::render::CurrentDrawResolveStatus::Ready"
        )
        end = DEVICE.index(
            "} else if (liveRebuildUsed && !liveRebuiltPalette.empty())", start
        )
        block = DEVICE[start:end]
        self.assertIn(
            "out.runtimeGroupPalette = std::move(directCurrentDrawSample.palette)",
            block,
        )
        self.assertIn(
            "resource.ownedVertexGroupIndices =\n"
            "            std::move(directCurrentDrawSample.groupSlots)",
            block,
        )
        self.assertNotIn(
            "outDirectCurrentDrawSample->palette = directCurrentDrawSample.palette",
            block,
        )
        self.assertNotIn(
            "outDirectCurrentDrawSample->groupSlots =", block
        )
        for token in (
            "outDirectCurrentDrawSample->contract = directCurrentDrawSample.contract",
            "outDirectCurrentDrawSample->paletteCount =",
            "outDirectCurrentDrawSample->paletteHash =",
            "outDirectCurrentDrawSample->groupHash =",
            "outDirectCurrentDrawSample->stableGroupHash =",
            "outDirectCurrentDrawSample->status = directCurrentDrawSample.status",
        ):
            self.assertIn(token, block)

    def test_live_rebuild_packet_is_the_only_palette_payload_owner(self) -> None:
        start = DEVICE.index(
            "} else if (liveRebuildUsed && !liveRebuiltPalette.empty())"
        )
        end = DEVICE.index("} else if (outDirectCurrentDrawSample != nullptr)", start)
        block = DEVICE[start:end]
        self.assertIn(
            "out.runtimeGroupPalette = std::move(liveRebuiltPalette)", block
        )
        self.assertNotIn(
            "outDirectCurrentDrawSample->palette = out.runtimeGroupPalette",
            block,
        )
        self.assertIn(
            "outDirectCurrentDrawSample->paletteHash = liveRebuiltHash", block
        )

    def test_append_prefers_packet_payload_and_diagnostics_use_same_palette(self) -> None:
        start = DEVICE.index(
            "bool D3D9DeviceEx::War3TryAppendSemanticShadowPacket(\n"
            "    const dxvk::war3::shadow::ShadowDrawPacket& packet,\n"
            "    const dxvk::war3::render::CurrentDrawAuthoritativeSample*"
        )
        end = DEVICE.index("uint32_t D3D9DeviceEx::War3TryPopulateDirectCurrentDrawGrouped", start)
        block = DEVICE[start:end]
        self.assertIn(
            "packetAuthoritativeSkinnedContractReady\n"
            "          ? &packet.runtimeGroupPalette",
            block,
        )
        self.assertIn(
            "packetAuthoritativeSkinnedContractReady ||\n"
            "        (currentDrawSample != nullptr && currentDrawSample->groupSlotsReady())",
            block,
        )
        self.assertIn(
            "drawTimeCapturedPaletteReady && drawTimeCapturedPalette != nullptr",
            block,
        )
        self.assertIn("(*drawTimeCapturedPalette)[0]", block)


if __name__ == "__main__":
    unittest.main()
