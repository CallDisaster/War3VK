#!/usr/bin/env python3
"""Static contracts for the runtime Stage11 package evidence observer."""

from __future__ import annotations

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src/d3d9/war3/gpu_skin/war3_persistent_gpu_package_stage11_observe_adapter.h"
SOURCE = ROOT / "src/d3d9/war3/gpu_skin/war3_persistent_gpu_package_stage11_observe_adapter.cpp"
RUNNABLE = ROOT / "src/d3d9/war3/gpu_skin/tests/war3_persistent_gpu_package_stage11_observe_adapter_test.cpp"
DEVICE_H = ROOT / "src/d3d9/d3d9_device.h"
DEVICE_CPP = ROOT / "src/d3d9/d3d9_device.cpp"
MESON = ROOT / "src/d3d9/meson.build"


class PersistentPackageStage11ObserveAdapterContracts(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.header = HEADER.read_text(encoding="utf-8")
        cls.source = SOURCE.read_text(encoding="utf-8")
        cls.runnable = RUNNABLE.read_text(encoding="utf-8")
        cls.device_h = DEVICE_H.read_text(encoding="utf-8")
        cls.device = DEVICE_CPP.read_text(encoding="utf-8")
        cls.meson = MESON.read_text(encoding="utf-8")

    def test_default_off_and_consume_are_hard_gates(self) -> None:
        for token in (
            "Off = 0u",
            "Observe = 1u",
            "Consume = 2u",
            "kObserveOnly = true",
            "kConsumeAdmissionGranted = false",
            "kBindsGpuResources = false",
            "kRecordsCommands = false",
            "kMutatesCanonicalDraw = false",
            "kPublishesPackage = false",
            "kProvesCurrentGameMemory = false",
        ):
            with self.subTest(token=token):
                self.assertIn(token, self.header)
        mode = self.device.split(
            "War3PersistentPackageStage11EvidenceModeRuntime()", 1
        )[1].split("}", 1)[0]
        self.assertIn("DXVK_WAR3_PERSISTENT_GPU_PACKAGE_STAGE11_EVIDENCE_MODE", mode)
        self.assertIn(", 0u)", mode)
        self.assertIn("Mode::Off", self.device)
        self.assertIn("Mode::Consume", self.device)

    def test_adapter_is_cpu_value_only_and_constant_space(self) -> None:
        implementation = self.header + self.source
        for forbidden in (
            "Rc<",
            "VkBuffer",
            "DxvkBuffer",
            "VkImage",
            "EmitCs",
            "RecordingAuthority",
            "PersistentGpuPackageStore",
            "ShadowArena",
            "std::vector",
            "std::unordered_map",
            "new ",
            "malloc(",
        ):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, implementation)
        self.assertIn("std::is_trivially_copyable_v", self.header)
        self.assertIn("std::atomic<uint64_t>", self.source)

    def test_process_monotonic_source_issuer_cannot_wrap(self) -> None:
        issuer = self.source.split(
            "issueCurrentStageSourceGeneration()", 1
        )[1].split("rollTimedFrame", 1)[0]
        self.assertIn("g_nextCurrentStageSourceGeneration", issuer)
        self.assertIn("compare_exchange_weak", issuer)
        self.assertIn("numeric_limits<uint64_t>::max", issuer)
        self.assertIn("return 0u", issuer)
        self.assertIn("b.sourceGeneration > a.sourceGeneration", self.runnable)

    def test_identity_matches_the_exact_producer_authority(self) -> None:
        validator = self.source.split(
            "validExactSubmittedWitness(", 1
        )[1].split("validExplicitSidecar", 1)[0]
        self.assertIn(
            "witness.instanceIdentity != 0u || witness.jHandle != 0u",
            validator,
        )
        self.assertIn("witness.meshPayloadIdentity != 0u", validator)
        self.assertIn("witness.renderablePartIdentity != 0u", validator)

    def test_only_csm_is_requested_and_no_consumer_is_claimed(self) -> None:
        self.assertIn(
            "ConsumerCsm0 | ConsumerCsm1 | ConsumerCsm2 | ConsumerCsm3",
            self.header,
        )
        evidence = self.source.split(
            "These are hard ceilings in the Observe adapter", 1
        )[1].split("if (requestedMode == Mode::Off)", 1)[0]
        for token in (
            "evidence.packageGeneration = 0u",
            "evidence.eligibleConsumerMask = 0u",
            "evidence.wouldUseConsumerMask = 0u",
            "evidence.actualConsumerMask = 0u",
            "evidence.packageReady = false",
            "evidence.fullyEquivalent = false",
            "evidence.drawMutationAllowed = false",
            "evidence.gpuBindingAllowed = false",
            "evidence.commandRecordingAllowed = false",
            "evidence.packagePublicationAllowed = false",
            "evidence.provesCurrentGameMemory = false",
        ):
            with self.subTest(token=token):
                self.assertIn(token, evidence)

    def test_hook_is_after_final_caster_and_exact_submitted_witness(self) -> None:
        function = self.device.split(
            "uint32_t D3D9DeviceEx::War3TryPopulateDrawTimeSemanticProducer()", 1
        )[1].split("bool D3D9DeviceEx::War3DrainShadowCasterTombstones()", 1)[0]
        instances = function.index("m_war3Scene.shadowInstances.emplace_back")
        casters = function.index("m_war3Scene.shadowCasters.emplace_back")
        exact = function.index(
            "entry.exactSubmittedFrameSerial = m_war3ShadowPersistentFrameSerial"
        )
        observe = function.index("War3ObservePersistentPackageStage11Evidence(")
        self.assertEqual(
            function.count("War3ObservePersistentPackageStage11Evidence("), 1
        )
        self.assertLess(instances, exact)
        self.assertLess(casters, exact)
        self.assertLess(exact, observe)
        mode = function.index(
            "War3PersistentPackageStage11EvidenceModeRuntime()"
        )
        loop = function.index("for (auto& [cacheKey, entry]")
        off_gate = function.index(
            "War3PersistentGpuPackageStage11ObserveAdapter::Mode::Off"
        )
        self.assertLess(mode, loop)
        self.assertLess(off_gate, observe)

    def test_runtime_sidecar_is_one_exact_o1_lookup(self) -> None:
        bridge = self.device.split(
            "void D3D9DeviceEx::War3ObservePersistentPackageStage11Evidence(", 1
        )[1].split(
            "uint32_t D3D9DeviceEx::War3TryPopulateDrawTimeSemanticProducer()", 1
        )[0]
        self.assertEqual(bridge.count("findGeosetStampByData("), 1)
        for forbidden in (
            "findGeosetSnapshotByData",
            "findGeosetByData",
            "hydrateGeosetByKnownPtrs",
            "War3PersistentGpuPackageRecordingAuthority",
            "War3PersistentGpuPackageStore",
            "EmitCs(",
            "ShadowArena_",
            "createBuffer",
            "allocateStorage",
            "m_war3Scene",
            "shadowCasters",
            "shadowInstances",
        ):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, bridge)
        self.assertIn("MissingExplicitGeosetDataSidecar", self.header)
        self.assertIn("ExplicitGeosetDataSidecarLookupFailed", self.header)
        self.assertIn("meshPayloadPtr", bridge)
        self.assertIn("try {", bridge)
        self.assertIn("catch (...)", bridge)
        self.assertIn("already-published canonical", bridge)

    def test_observe_times_only_the_core_lookup_and_adapter_call(self) -> None:
        bridge = self.device.split(
            "void D3D9DeviceEx::War3ObservePersistentPackageStage11Evidence(", 1
        )[1].split(
            "uint32_t D3D9DeviceEx::War3TryPopulateDrawTimeSemanticProducer()", 1
        )[0]
        begin = bridge.index("high_resolution_clock::get_counter()")
        lookup = bridge.index("findGeosetStampByData(")
        observe = bridge.index(".observe(")
        note = bridge.index(".noteElapsedTicks(")
        self.assertLess(begin, lookup)
        self.assertLess(lookup, observe)
        self.assertLess(observe, note)
        self.assertIn("avgCoreCallUs", bridge)
        self.assertIn("provesCurrentGameMemory=0", bridge)
        for token in (
            "elapsedTicksTotal",
            "elapsedTicksMaxCall",
            "elapsedTicksMaxFrame",
        ):
            with self.subTest(token=token):
                self.assertIn(token, self.header)

    def test_content_join_never_claims_current_game_memory(self) -> None:
        implementation = self.header + self.source + self.runnable
        self.assertIn("RecordedContentIdentityOnly", implementation)
        self.assertIn("kProvesCurrentGameMemory = false", self.header)
        self.assertIn("provesCurrentGameMemory = false", self.header)
        self.assertNotIn("RecordedInputEvidence", implementation)
        self.assertIn("!observed.provesCurrentGameMemory", self.runnable)
        self.assertIn("std::atomic<bool> s_consumeDeniedLogged", self.device)

    def test_meson_compiles_production_and_isolated_runnable(self) -> None:
        for token in (
            "'war3/gpu_skin/war3_persistent_gpu_package_stage11_observe_adapter.cpp'",
            "'war3_persistent_gpu_package_stage11_observe_adapter_test'",
            "'war3/gpu_skin/tests/war3_persistent_gpu_package_stage11_observe_adapter_test.cpp'",
        ):
            with self.subTest(token=token):
                self.assertIn(token, self.meson)


if __name__ == "__main__":
    unittest.main(verbosity=2)
