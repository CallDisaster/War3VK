#!/usr/bin/env python
# -*- coding: utf-8 -*-

import tempfile
import unittest
from pathlib import Path
from unittest import mock

from autotest_sessions import (
    AutoTestSession,
    SessionRegistry,
    build_instance_layout,
    deploy_content_addressed_map,
    is_path_within,
    materialize_instance_root,
    preflight_instance_pool,
    sha256_file,
)


class PathAndLayoutTests(unittest.TestCase):
    def test_path_boundary_rejects_prefix_and_parent_escape(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / "pool"
            self.assertTrue(is_path_within(root / "run" / "client", root))
            self.assertFalse(is_path_within(root.parent / "pool-evil" / "client", root))
            self.assertFalse(is_path_within(root / ".." / "outside", root))

    def test_layouts_are_run_and_session_isolated(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            first = build_instance_layout(base / "sandbox", base / "artifacts", "run-a", "client-01")
            second = build_instance_layout(base / "sandbox", base / "artifacts", "run-a", "client-02")
            self.assertNotEqual(first.instance_root, second.instance_root)
            self.assertNotEqual(first.artifact_dir, second.artifact_dir)
            self.assertIn("client-01", first.desktop_name)

    def test_preflight_is_read_only_and_reports_missing_files(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            sandbox = Path(temp) / "sandbox"
            sandbox.mkdir()
            artifacts = Path(temp) / "artifacts"
            result = preflight_instance_pool(sandbox, artifacts, None, 2, "run-a")
            self.assertFalse(result["ok"])
            self.assertFalse((sandbox / "_AutoTestInstances").exists())
            self.assertFalse(artifacts.exists())

    def test_preflight_rejects_prefix_that_cannot_fit_number_suffix(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            sandbox = Path(temp) / "sandbox"
            sandbox.mkdir()
            result = preflight_instance_pool(
                sandbox,
                Path(temp) / "artifacts",
                None,
                2,
                "run-a",
                "x" * 61,
            )
            self.assertFalse(result["ok"])
            self.assertTrue(any("最长为 60" in error for error in result["errors"]))


class MaterializationTests(unittest.TestCase):
    def _make_sandbox(self, root: Path) -> None:
        root.mkdir(parents=True)
        for name in ("war3.exe", "Game.dll"):
            (root / name).write_bytes(name.encode("ascii"))
        for name in ("war3.mpq", "war3x.mpq", "war3patch.mpq"):
            (root / name).write_bytes(("mpq:" + name).encode("ascii"))
        (root / "WarVK").mkdir()
        (root / "WarVK" / "runtime.dll").write_bytes(b"runtime")
        (root / "WarVK" / "Log").mkdir()
        (root / "WarVK" / "Log" / "old.log").write_text("old", encoding="utf-8")

    def test_mpq_hardlinks_and_mutable_files_are_copied(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            sandbox = base / "sandbox"
            self._make_sandbox(sandbox)
            layout = build_instance_layout(sandbox, base / "artifacts", "run-a", "client-01")
            result = materialize_instance_root(layout)
            self.assertTrue(result["ok"], result)
            self.assertEqual((sandbox / "war3.mpq").stat().st_ino, (layout.instance_root / "war3.mpq").stat().st_ino)
            self.assertNotEqual((sandbox / "Game.dll").stat().st_ino, (layout.instance_root / "Game.dll").stat().st_ino)
            self.assertFalse((layout.instance_root / "WarVK" / "Log" / "old.log").exists())

    def test_map_is_content_addressed(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            sandbox = base / "sandbox"
            self._make_sandbox(sandbox)
            layout = build_instance_layout(sandbox, base / "artifacts", "run-a", "client-01")
            self.assertTrue(materialize_instance_root(layout)["ok"])
            source = base / "地图.w3x"
            source.write_bytes(b"map-data")
            result = deploy_content_addressed_map(layout.instance_root, source)
            self.assertTrue(result["ok"])
            self.assertIn(sha256_file(source)[:16], result["target"])
            self.assertEqual(sha256_file(Path(result["target"])), result["sha256"])


class RegistryTests(unittest.TestCase):
    def _session(self, session_id: str) -> AutoTestSession:
        return AutoTestSession(
            session_id=session_id,
            run_id="run-a",
            sandbox_root=Path("sandbox"),
            instance_root=Path("instance") / session_id,
            artifact_dir=Path("artifacts") / session_id,
        )

    def test_pid_routing_is_session_local(self) -> None:
        registry = SessionRegistry()
        registry.reserve(self._session("client-01"))
        registry.reserve(self._session("client-02"))
        registry.attach_pid("client-01", 101)
        registry.attach_pid("client-02", 202)
        self.assertTrue(registry.route_event(202, "DXVK client two"))
        self.assertEqual(registry.require("client-01").get_events(0, 10), [])
        self.assertEqual(registry.require("client-02").get_events(0, 10)[0]["pid"], 202)

    def test_orphan_cleanup_only_marks_dead_process(self) -> None:
        registry = SessionRegistry()
        registry.reserve(self._session("client-01"))
        registry.reserve(self._session("client-02"))
        registry.attach_pid("client-01", 101)
        registry.attach_pid("client-02", 202)
        cleaned = registry.cleanup_orphans(lambda pid: pid == 101)
        self.assertEqual([row.session_id for row in cleaned], ["client-02"])
        self.assertEqual(registry.require("client-01").state, "running")
        self.assertEqual(registry.require("client-02").state, "orphaned")


class McpBoundaryTests(unittest.TestCase):
    def test_multi_instance_api_rejects_non_sandbox_root(self) -> None:
        import war3_autotest_mcp as mcp

        result = mcp.preflight_instance_pool(
            sandbox_root=r"E:\Work\War3",
            map_path="",
            instance_count=2,
            run_id="boundary-test",
        )
        self.assertFalse(result["ok"])
        self.assertIn("只允许使用专用沙盒", result["errors"][0])

    def test_batch_stop_requires_explicit_scope(self) -> None:
        import war3_autotest_mcp as mcp

        result = mcp.stop_war3_batch()
        self.assertFalse(result["ok"])
        self.assertIn("必须提供", result["error"])

    def test_lan_suite_never_falls_back_to_independent_loadfile(self) -> None:
        import war3_autotest_mcp as mcp

        result = mcp.run_multi_instance_suite(
            sandbox_root=r"E:\Work\War3",
            map_path="",
            instance_count=2,
            run_id="lan-boundary-test",
        )
        self.assertFalse(result["ok"])
        self.assertEqual(result["code"], "INSTANCE_PREFLIGHT_FAILED")
        self.assertFalse(result["countsAsLanAcceptance"])

    def test_lan_launch_requires_opt_in_then_stops_at_protocol_blocker(self) -> None:
        import war3_autotest_mcp as mcp

        instance_check = {"ok": True, "runId": "lan-opt-in", "instanceCount": 2}
        ydhost_check = {"ok": True, "code": "YDHOST_PREFLIGHT_READY"}
        with mock.patch.object(mcp, "_preflight_instance_pool_impl", return_value=instance_check), mock.patch.object(
            mcp, "_preflight_ydhost_lan", return_value=ydhost_check
        ):
            disabled = mcp.run_multi_instance_suite(
                sandbox_root=str(mcp.DEFAULT_SANDBOX_ROOT),
                map_path=str(mcp.DEFAULT_SANDBOX_ROOT / "Maps" / "fake.w3x"),
                instance_count=2,
                run_id="lan-opt-in",
            )
            self.assertFalse(disabled["ok"])
            self.assertEqual("YDHOST_LAUNCH_OPT_IN_REQUIRED", disabled["code"])
            self.assertFalse(disabled["realProcessLaunchExecuted"])

            opted_in = mcp.run_multi_instance_suite(
                sandbox_root=str(mcp.DEFAULT_SANDBOX_ROOT),
                map_path=str(mcp.DEFAULT_SANDBOX_ROOT / "Maps" / "fake.w3x"),
                instance_count=2,
                run_id="lan-opt-in",
                enable_ydhost_launch=True,
            )
            self.assertFalse(opted_in["ok"])
            self.assertEqual("YDHOST_CLIENT_LAUNCH_PROTOCOL_UNRESOLVED", opted_in["code"])
            self.assertFalse(opted_in["realProcessLaunchExecuted"])


if __name__ == "__main__":
    unittest.main()
