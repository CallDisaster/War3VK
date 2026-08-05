#!/usr/bin/env python3
"""Static safety contracts for the value-only package generation owner."""

from __future__ import annotations

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
OWNER_H = ROOT / "src/d3d9/war3/gpu_skin/war3_persistent_gpu_package_owner.h"
OWNER_CPP = ROOT / "src/d3d9/war3/gpu_skin/war3_persistent_gpu_package_owner.cpp"
STORE_H = ROOT / "src/d3d9/war3/gpu_skin/war3_persistent_gpu_package_store.h"
STORE_CPP = ROOT / "src/d3d9/war3/gpu_skin/war3_persistent_gpu_package_store.cpp"
DEVICE_CPP = ROOT / "src/d3d9/d3d9_device.cpp"
MESON = ROOT / "src/d3d9/meson.build"


class PersistentPackageGenerationOwnerContracts(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.owner_h = OWNER_H.read_text(encoding="utf-8")
        cls.owner_cpp = OWNER_CPP.read_text(encoding="utf-8")
        cls.store = STORE_H.read_text(encoding="utf-8") + STORE_CPP.read_text(
            encoding="utf-8"
        )
        cls.device = DEVICE_CPP.read_text(encoding="utf-8")
        cls.meson = MESON.read_text(encoding="utf-8")

    def test_generation_record_carries_both_completion_proofs(self) -> None:
        record = self.owner_h.split("struct GenerationRetirement", 1)[1].split(
            "};", 1
        )[0]
        for token in (
            "GenerationKey key;",
            "FencePoint uploadFence;",
            "ConsumerLastUse consumerLastUse;",
            "retirementSerial",
            "retirementRequested",
        ):
            with self.subTest(token=token):
                self.assertIn(token, record)

        consumer = self.owner_h.split("struct ConsumerLastUse", 1)[1].split(
            "};", 1
        )[0]
        for token in ("FencePoint fence;", "consumerMask", "submitSerial"):
            with self.subTest(token=token):
                self.assertIn(token, consumer)

    def test_zero_and_unknown_proofs_fail_closed(self) -> None:
        self.assertIn("point.fenceIdentity != 0u && point.value != 0u", self.owner_cpp)
        self.assertIn("lastUse.consumerMask != 0u", self.owner_cpp)
        self.assertIn("lastUse.submitSerial != 0u", self.owner_cpp)
        self.assertIn("lastUse.consumerMask & ~kKnownConsumerMask", self.owner_cpp)
        self.assertIn("!uploadObservation.querySucceeded", self.owner_cpp)
        self.assertIn("!consumerObservation.querySucceeded", self.owner_cpp)

    def test_epoch_retirement_requires_upload_and_consumer_proofs(self) -> None:
        retire = self.owner_cpp.split("requestEpochRetirement(", 1)[1].split(
            "tryReclaim(", 1
        )[0]
        upload_gate = retire.index("validateFencePoint(retirement.uploadFence)")
        consumer_gate = retire.index(
            "validateConsumerLastUse(retirement.consumerLastUse)"
        )
        retirement_write = retire.index("retirement.retirementRequested = true")
        self.assertLess(upload_gate, retirement_write)
        self.assertLess(consumer_gate, retirement_write)

        reclaim = self.owner_cpp.split("tryReclaim(", 1)[1].split(
            "inspect(", 1
        )[0]
        self.assertIn("RetainedUploadProofMissing", reclaim)
        self.assertIn("RetainedConsumerProofMissing", reclaim)
        self.assertIn("RetainedUploadPending", reclaim)
        self.assertIn("RetainedConsumerPending", reclaim)
        self.assertLess(reclaim.index("RetainedUploadPending"), reclaim.index("erase(found)"))
        self.assertLess(reclaim.index("RetainedConsumerPending"), reclaim.index("erase(found)"))

    def test_generation_and_fence_identity_mismatches_retain(self) -> None:
        reclaim = self.owner_cpp.split("tryReclaim(", 1)[1].split(
            "inspect(", 1
        )[0]
        self.assertIn("RetainedGenerationMismatch", reclaim)
        self.assertIn("RetainedFenceMismatch", reclaim)
        self.assertEqual(reclaim.count("m_generations.erase(found)"), 1)

    def test_observe_and_shared_consume_remain_disabled(self) -> None:
        for token in (
            "kRuntimeObserveEnabled = false",
            "kObserveValidationOnly = true",
            "kObserveBindsAtlas = false",
            "kObserveWritesConsumerLastUse = false",
            "kSharedConsumerEnabled = false",
        ):
            with self.subTest(token=token):
                self.assertIn(token, self.owner_h)
        observe_declaration = self.owner_h.split(
            "observeConsumerLastUseCandidate(", 1
        )[1].split(";", 1)[0]
        self.assertIn("const noexcept", observe_declaration)
        observe = self.owner_cpp.split(
            "observeConsumerLastUseCandidate(", 1
        )[1].split("publishConsumerLastUse(", 1)[0]
        for mutation_pattern in (
            r"stored\.consumerMask\s*\|=",
            r"stored\.consumerMask\s*=(?!=)",
            r"stored\.fence\s*=(?!=)",
            r"stored\.submitSerial\s*=(?!=)",
            r"m_generations\.emplace",
        ):
            with self.subTest(mutation_pattern=mutation_pattern):
                self.assertNotRegex(observe, mutation_pattern)
        self.assertIn("kD3D9SharedOwnerEnabled = false", self.store)
        self.assertIn("kCrossEpochRetirementSafe = false", self.store)

    def test_owner_is_value_only_and_not_integrated(self) -> None:
        implementation = self.owner_h + self.owner_cpp
        for forbidden in (
            "DxvkBuffer",
            "DxvkFence",
            "VkBuffer",
            "Rc<",
            "staticAtlasSlice",
            "d3d9_device",
            "ShadowArena",
        ):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, implementation)
        self.assertNotIn("War3PersistentGpuPackageOwner", self.store)
        self.assertNotIn("War3PersistentGpuPackageOwner", self.device)

    def test_production_and_isolated_test_targets_compile_the_owner(self) -> None:
        self.assertIn(
            "'war3/gpu_skin/war3_persistent_gpu_package_owner.cpp'",
            self.meson,
        )
        self.assertIn(
            "'war3_persistent_gpu_package_generation_owner_test'",
            self.meson,
        )
        self.assertIn(
            "'war3/gpu_skin/tests/war3_persistent_gpu_package_generation_owner_test.cpp'",
            self.meson,
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
