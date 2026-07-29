#!/usr/bin/env python
# -*- coding: utf-8 -*-

import hashlib
import tempfile
import unittest
from pathlib import Path

from ydwe_lan_protocol import (
    AUDITED_RUNTIME_IDENTITIES,
    CLIENT_PROTOCOL_BLOCKER,
    RuntimeIdentity,
    audit_runtime_identity,
    build_client_command_plan,
    build_lan_command_plan,
    describe_protocol,
    identity_contract,
)


class YdweLanProtocolTests(unittest.TestCase):
    def test_checked_in_distribution_matches_identity_contract(self) -> None:
        repo = Path(__file__).resolve().parents[5]
        root = repo / "SourceMap" / "YDWE1.32.13 - MemoryHack"
        result = audit_runtime_identity(root)
        self.assertTrue(result["ok"], result)
        self.assertEqual(len(AUDITED_RUNTIME_IDENTITIES), len(result["files"]))

    def test_identity_audit_rejects_content_drift(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            target = root / "bin" / "sample.dll"
            target.parent.mkdir(parents=True)
            target.write_bytes(b"trusted")
            expected = RuntimeIdentity(
                "bin/sample.dll",
                len(b"trusted"),
                hashlib.sha256(b"trusted").hexdigest(),
                "test",
            )
            self.assertTrue(audit_runtime_identity(root, (expected,))["ok"])
            target.write_bytes(b"drifted")
            rejected = audit_runtime_identity(root, (expected,))
            self.assertFalse(rejected["ok"])
            self.assertTrue(any("漂移" in error for error in rejected["errors"]))

    def test_protocol_does_not_claim_auto_is_native_or_ready(self) -> None:
        protocol = describe_protocol()
        self.assertFalse(protocol["autoIsNativeWar3Argument"])
        self.assertIn("不能", protocol["autoEnterStateMachine"]["inGameReady"])
        self.assertIn("DBWIN/JAPI", protocol["readyEvidenceRequired"])
        self.assertGreaterEqual(len(protocol["sourceEvidence"]), 6)

    def test_client_plan_is_auditable_but_fail_closed(self) -> None:
        repo = Path(__file__).resolve().parents[5]
        ydwe = repo / "SourceMap" / "YDWE1.32.13 - MemoryHack"
        result = build_client_command_plan(
            ydwe,
            Path(r"E:\Work\War3_AutoTestSandbox\_AutoTestInstances\run-a\client-01\root"),
            "client-01",
            desktop_name="War3AutoTest_run-a_client-01",
            job_id="job-client-01",
        )
        self.assertFalse(result["ok"])
        self.assertFalse(result["launchable"])
        self.assertEqual(CLIENT_PROTOCOL_BLOCKER, result["code"])
        self.assertEqual(["-war3", "-closew2l", "-auto"], result["wrapperCommand"][1:])
        self.assertNotIn("-loadfile", result["wrapperCommand"])
        self.assertTrue(result["identity"]["ok"], result)
        self.assertFalse(result["realProcessLaunchExecuted"])
        codes = {row["code"] for row in result["blockers"]}
        self.assertIn("YDWE_PER_INSTANCE_WAR3_ROOT_UNRESOLVED", codes)
        self.assertIn("YDWE_AUTO_READY_SIGNAL_ABSENT", codes)

    def test_multi_client_plan_requires_unique_roots(self) -> None:
        repo = Path(__file__).resolve().parents[5]
        ydwe = repo / "SourceMap" / "YDWE1.32.13 - MemoryHack"
        with self.assertRaisesRegex(ValueError, "独立 YDWE"):
            build_lan_command_plan(
                [ydwe, ydwe],
                [Path("C:/war3-a"), Path("C:/war3-b")],
                "run-a",
            )

    def test_identity_contract_never_authorizes_production(self) -> None:
        contract = identity_contract()
        self.assertFalse(contract["productionLaunchAllowed"])
        self.assertEqual(CLIENT_PROTOCOL_BLOCKER, contract["blocker"])
        self.assertEqual(len(AUDITED_RUNTIME_IDENTITIES), len(contract["runtimeFiles"]))


if __name__ == "__main__":
    unittest.main()
