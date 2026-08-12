import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(
    encoding="utf-8", errors="ignore"
)
HEADER = (ROOT / "src/d3d9/d3d9_device.h").read_text(
    encoding="utf-8", errors="ignore"
)


def function_block(signature: str, next_signature: str) -> str:
    start = DEVICE.index(signature)
    end = DEVICE.index(next_signature, start + len(signature))
    return DEVICE[start:end]


class DirectExactManifestHandoffContracts(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.producer = function_block(
            "uint32_t D3D9DeviceEx::War3TryPopulateDrawTimeSemanticProducer(",
            "void D3D9DeviceEx::War3CollectRetiredShadowSessions(",
        )
        cls.grouped = function_block(
            "uint32_t D3D9DeviceEx::War3TryPopulateDirectCurrentDrawGrouped(",
            "uint32_t D3D9DeviceEx::War3TryPopulateSemanticShadowScene(",
        )
        populate_start = DEVICE.index(
            "uint32_t D3D9DeviceEx::War3TryPopulateSemanticShadowScene("
        )
        cls.populate = DEVICE[populate_start : DEVICE.index(
            "bool D3D9DeviceEx::War3ExecuteSemanticShadowSceneForValidation(",
            populate_start,
        )]

    def test_handoff_is_call_owned_and_not_a_device_cache(self) -> None:
        declaration = (
            "std::vector<dxvk::war3::render::CurrentDrawContractRecord>&\n"
            "        exactSubmittedManifestRecords"
        )
        self.assertIn(declaration, self.producer)
        self.assertIn("exactSubmittedManifestRecords.clear()", self.producer)
        self.assertNotIn("m_war3ExactSubmittedManifest", HEADER)
        self.assertNotIn("static thread_local", self.producer)

    def test_producer_republishes_same_frame_exact_records(self) -> None:
        owner_gate = self.producer.index(
            "entry.exactOwnerFrameSerial == m_war3ShadowPersistentFrameSerial"
        )
        submitted_gate = self.producer.index(
            "entry.exactSubmittedFrameSerial ==", owner_gate
        )
        append = self.producer.index(
            "appendExactSubmittedManifestRecord(", submitted_gate
        )
        early_continue = self.producer.index("continue;", append)
        self.assertLess(owner_gate, submitted_gate)
        self.assertLess(submitted_gate, append)
        self.assertLess(append, early_continue)

    def test_new_submission_appends_after_positive_publish_marker(self) -> None:
        marker = self.producer.index(
            "entry.exactSubmittedFrameSerial = m_war3ShadowPersistentFrameSerial"
        )
        append = self.producer.index(
            "appendExactSubmittedManifestRecord(", marker
        )
        self.assertLess(marker, append)
        self.assertIn("producerFreshThisFrame = true", self.producer)
        self.assertIn("sourceKind =\n        dxvk::war3::render::ShadowProducerKind::DrawTimeGeometry", self.producer)
        self.assertIn("alphaPayloadComplete", self.producer)

    def test_grouped_consumer_no_longer_rescans_draw_time_cache(self) -> None:
        self.assertIn("const std::vector<dxvk::war3::render::CurrentDrawContractRecord>&", self.grouped)
        self.assertNotIn(
            "for (const auto& [cacheKey, entry] : m_war3DrawTimeVBCache)",
            self.grouped,
        )
        self.assertIn(
            "publishShadowManifestSummary(exactSubmittedManifestRecords,\n"
            "                               shadowEligibleManifestRecords)",
            self.grouped,
        )
        self.assertNotIn(
            "shadowEligibleManifestRecords.insert(\n"
            "      shadowEligibleManifestRecords.end(),\n"
            "      exactSubmittedManifestRecords.begin()",
            self.grouped,
        )

    def test_caller_passes_one_value_snapshot_to_both_producers(self) -> None:
        declaration = self.populate.index("exactSubmittedManifestRecords;")
        producer_call = self.populate.index(
            "War3TryPopulateDrawTimeSemanticProducer(\n"
            "          exactSubmittedManifestRecords)",
            declaration,
        )
        grouped_call = self.populate.index(
            "currentDrawMinVisibleFrameSerial,\n"
            "        exactSubmittedManifestRecords)",
            producer_call,
        )
        self.assertLess(declaration, producer_call)
        self.assertLess(producer_call, grouped_call)


if __name__ == "__main__":
    unittest.main()
