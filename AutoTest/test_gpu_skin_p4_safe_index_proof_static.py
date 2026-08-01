#!/usr/bin/env python3
"""Static/pure contracts for the P4 B1 safe index-proof blocker.

The tests do not build, deploy, or launch Warcraft III.  They protect the
diagnostic boundary that keeps the legacy cross-frame DrawTime cache disabled
while distinguishing an unavailable current-frame index proof from a genuine
runtime failure.
"""

from __future__ import annotations

import importlib.util
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / "AutoTest/run_gpu_skin_p4_isolated.py"
D3D9_DEVICE = ROOT / "src/d3d9/d3d9_device.cpp"


def _load_runner():
    spec = importlib.util.spec_from_file_location(
        "warvk_p4_safe_index_runner", RUNNER
    )
    if spec is None or spec.loader is None:
        raise RuntimeError("unable to load P4 runner module")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


P4 = _load_runner()


def _clean_gates() -> dict[str, bool]:
    return {
        name: True for name in P4.SAFE_INDEX_PROOF_REQUIRED_CLEAN_GATES
    }


def _blocked_diag() -> dict[str, object]:
    return {
        "VSRoute": {
            "route": 3,
            "explicit": 1,
            "invalid": 0,
            "inputPrepared": [120, 5760],
            "inputSubmitted": [120, 5760],
            "inputOnly": [120, 5760, 120, 491520, 0, 0],
            "main": [0, 0, 0, 0],
            "shadowCapture": [0, 0, 0, 0],
            "shadowDirect": [0, 0, 0, 0, 0],
            "shadowReplay": [0, 0, 0],
        },
        "P3": {
            "bypassAttempts": 120,
            "bypassAuthorizations": 0,
            "bypassCommits": 0,
            "bypassFallbacks": 120,
        },
        "P4Shadow": {
            "leasesConsumed": 0,
            "preflightIndexReject": 118,
            "preflightUvReject": 0,
            "finalPositionReject": 0,
            "finalIndexReject": 0,
            "finalUvReject": 0,
            "finalCommitReject": 0,
            "bypassCommits": 0,
        },
        "indexTicket": {
            "mask": 0,
            "attempts": 0,
            "exact": 0,
            "suppressed": 0,
            "leaks": 0,
        },
        "kernel": {
            "hookCalls": 400,
            "originalCalls": 400,
            "bypassedCalls": 0,
            "bypassedBytes": 0,
        },
    }


def _launch(cache_value: str = "0") -> dict[str, object]:
    return {
        "effectiveWar3Environment": {
            P4.GPU_SKIN_EXECUTION_ROUTE_ENV: "vertex_shader_bypass",
            P4.LEGACY_DRAWTIME_INDEX_CACHE_ENV: cache_value,
        }
    }


class P4SafeIndexProofClassificationTests(unittest.TestCase):
    def classify(
        self,
        *,
        diag: dict[str, object] | None = None,
        gates: dict[str, bool] | None = None,
        launch: dict[str, object] | None = None,
        runtime_failure: bool = False,
        runtime_death: bool = False,
    ) -> dict[str, object]:
        return P4._classify_b1_safe_index_proof_blocker(
            diag or _blocked_diag(),
            gates or _clean_gates(),
            launch or _launch(),
            phase="crash-gate",
            execution_route="vertex_shader_bypass",
            error=None,
            runtime_failure_authoritative=runtime_failure,
            runtime_death_observed=runtime_death,
        )

    def test_exact_zero_authority_shape_is_safely_blocked(self) -> None:
        result = self.classify()
        self.assertTrue(result["blocked"])
        self.assertEqual(result["code"], "BLOCKED_SAFE_INDEX_PROOF")
        self.assertTrue(result["SafeIndexProofUnavailable"])
        self.assertTrue(result["LegacyIndexCacheDisabled"])
        self.assertTrue(result["safetyContractClean"])

    def test_enabling_legacy_cache_never_satisfies_safe_blocker(self) -> None:
        result = self.classify(launch=_launch("1"))
        self.assertFalse(result["blocked"])
        self.assertTrue(result["SafeIndexProofUnavailable"])
        self.assertFalse(result["LegacyIndexCacheDisabled"])

    def test_final_index_reject_remains_runtime_failure_shape(self) -> None:
        diag = _blocked_diag()
        diag["P4Shadow"] = dict(diag["P4Shadow"])
        diag["P4Shadow"]["finalIndexReject"] = 1
        result = self.classify(diag=diag)
        self.assertFalse(result["blocked"])
        self.assertFalse(result["SafeIndexProofUnavailable"])

    def test_authoritative_runtime_failure_takes_precedence(self) -> None:
        result = self.classify(runtime_failure=True)
        self.assertFalse(result["blocked"])
        self.assertTrue(result["runtimeFailureAuthorityPresent"])

    def test_lifecycle_or_process_safety_failure_takes_precedence(self) -> None:
        gates = _clean_gates()
        gates["lifetimeClean"] = False
        result = self.classify(gates=gates)
        self.assertFalse(result["blocked"])
        self.assertFalse(result["safetyContractClean"])
        self.assertIn("lifetimeClean", result["failedSafetyGates"])
        self.assertFalse(self.classify(runtime_death=True)["blocked"])


class P4SafeIndexProofSourceContractTests(unittest.TestCase):
    def test_runner_pins_legacy_cache_off_for_every_route(self) -> None:
        for route in P4.GPU_SKIN_EXECUTION_ROUTES:
            with self.subTest(route=route):
                environment = P4._p4_environment(
                    "light", "none", route
                )
                self.assertEqual(
                    environment[P4.LEGACY_DRAWTIME_INDEX_CACHE_ENV], "0"
                )

    def test_b1_runner_cannot_promote_by_reenabling_cross_frame_cache(self) -> None:
        source = RUNNER.read_text(encoding="utf-8")
        self.assertNotIn(
            'LEGACY_DRAWTIME_INDEX_CACHE_ENV: "1"', source
        )
        self.assertIn(
            'LEGACY_DRAWTIME_INDEX_CACHE_ENV: "0"', source
        )
        self.assertIn(
            "Publish a generation-pinned current-frame exact index proof",
            source,
        )

    def test_production_legacy_cache_default_remains_fail_closed(self) -> None:
        source = D3D9_DEVICE.read_text(encoding="utf-8")
        self.assertIn(
            'War3GetEnvU32("DXVK_WAR3_DRAWTIME_VB_CACHE", 0u)', source
        )

    def test_blocker_is_not_reported_as_pass_or_runtime_failure(self) -> None:
        source = RUNNER.read_text(encoding="utf-8")
        self.assertIn('"blockedSafeIndexProof": True', source)
        self.assertIn('"gpuSkinRuntimeFailure": False', source)
        self.assertIn("and not safe_index_proof_blocked", source)
        self.assertIn("return 0 if passed else 2 if safe_index_proof_blocked else 1", source)


if __name__ == "__main__":
    unittest.main(verbosity=2)
