#!/usr/bin/env python
# -*- coding: utf-8 -*-

import hashlib
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import ydhost_adapter

from ydhost_lan_adapter import (
    ClientHandle,
    HostHandle,
    LanDependencies,
    LanLaunchSpec,
    LanProbeSnapshot,
    PreparedLanRun,
    YdhostLanCoordinator,
    real_launch_capability,
)


class FakeClock:
    def __init__(self) -> None:
        self.value = 0.0

    def monotonic(self) -> float:
        return self.value

    def sleep(self, seconds: float) -> None:
        self.value += float(seconds)


class FakeRuntime:
    def __init__(
        self,
        root: Path,
        *,
        missing_evidence: bool = False,
        fail_client_index: int = 0,
        duplicate_job: bool = False,
        duplicate_pid: bool = False,
        host_dies: bool = False,
        raise_record_event: bool = False,
        fail_stop_client: str = "",
        fail_stop_host: bool = False,
    ) -> None:
        self.root = root
        self.missing_evidence = missing_evidence
        self.fail_client_index = fail_client_index
        self.duplicate_job = duplicate_job
        self.duplicate_pid = duplicate_pid
        self.host_dies = host_dies
        self.raise_record_event = raise_record_event
        self.fail_stop_client = fail_stop_client
        self.fail_stop_host = fail_stop_host
        self.probe_calls = 0
        self.calls: list[str] = []
        self.events = []
        self.stopped_clients: list[str] = []
        self.host_stops = 0

    def dependencies(self) -> LanDependencies:
        return LanDependencies(
            prepare=self.prepare,
            launch_host=self.launch_host,
            launch_client=self.launch_client,
            probe=self.probe,
            stop_client=self.stop_client,
            stop_host=self.stop_host,
            record_event=self.record_event,
        )

    def prepare(self, spec: LanLaunchSpec) -> PreparedLanRun:
        self.calls.append("prepare")
        run_root = self.root / spec.run_id / "ydhost-host"
        artifact_root = self.root / spec.run_id / "artifacts"
        run_root.mkdir(parents=True, exist_ok=True)
        artifact_root.mkdir(parents=True, exist_ok=True)
        config = b"ydhost-config"
        map_config = b"map-config"
        (run_root / "ydhost.cfg").write_bytes(config)
        (run_root / "map.cfg").write_bytes(map_config)
        return PreparedLanRun(
            run_id=spec.run_id,
            sandbox_root=self.root,
            run_root=run_root,
            artifact_root=artifact_root,
            ydhost_cfg=run_root / "ydhost.cfg",
            map_cfg=run_root / "map.cfg",
            evidence={
                "configSha256": hashlib.sha256(config).hexdigest(),
                "mapSha256": hashlib.sha256(map_config).hexdigest(),
            },
        )

    def launch_host(self, _prepared: PreparedLanRun) -> HostHandle:
        self.calls.append("launch-host")
        return HostHandle(pid=100, job_id="host-job", hidden=True)

    def launch_client(self, _prepared: PreparedLanRun, index: int, session_id: str) -> ClientHandle:
        self.calls.append(f"launch-{session_id}")
        if index == self.fail_client_index:
            raise RuntimeError(f"client {index} launch failed")
        job = "host-job" if self.duplicate_job and index == 1 else f"client-job-{index}"
        return ClientHandle(
            session_id=session_id,
            pid=100 if self.duplicate_pid and index == 1 else 200 + index,
            job_id=job,
            desktop_name=f"desktop-{index}",
        )

    def probe(
        self,
        _prepared: PreparedLanRun,
        _host: HostHandle,
        clients: list[ClientHandle],
    ) -> LanProbeSnapshot:
        self.probe_calls += 1
        self.calls.append("probe")
        sessions = frozenset(client.session_id for client in clients)
        if self.host_dies:
            return LanProbeSnapshot(host_alive=False)
        if not clients:
            listening = self.probe_calls >= 2
            return LanProbeSnapshot(
                host_alive=True,
                listening=listening,
                listening_evidence="ydhost.log:listening" if listening else "",
            )
        if self.missing_evidence:
            return LanProbeSnapshot(
                host_alive=True,
                listening=True,
                game_created=True,
                client_alive=sessions,
                joined_clients=sessions,
                ready_clients=sessions,
                raw={"booleansOnly": True},
            )
        ready = self.probe_calls >= 4
        joined = sessions if ready else frozenset(list(sessions)[:1])
        ready_sessions = sessions if ready else frozenset()
        return LanProbeSnapshot(
            host_alive=True,
            listening=True,
            game_created=True,
            client_alive=sessions,
            joined_clients=joined,
            ready_clients=ready_sessions,
            listening_evidence="ydhost.log:listening",
            game_created_evidence="ydhost.log:creating-game",
            joined_evidence={session: f"ydhost.log:{session}:joined" for session in joined},
            ready_evidence={session: f"dbwin:{session}:ready" for session in ready_sessions},
        )

    def stop_client(self, _prepared: PreparedLanRun, client: ClientHandle, reason: str):
        self.calls.append(f"stop-{client.session_id}")
        self.stopped_clients.append(client.session_id)
        if client.session_id == self.fail_stop_client:
            raise RuntimeError(f"stop failed: {client.session_id}")
        return {"ok": True, "sessionId": client.session_id, "reason": reason}

    def stop_host(self, _prepared: PreparedLanRun, _host: HostHandle, reason: str):
        self.calls.append("stop-host")
        self.host_stops += 1
        if self.fail_stop_host:
            raise RuntimeError("host stop failed")
        return {"ok": True, "component": "ydhost", "reason": reason}

    def record_event(self, _prepared: PreparedLanRun, event):
        if self.raise_record_event:
            raise RuntimeError("event sink failed")
        self.events.append(dict(event))


class LanStateMachineTests(unittest.TestCase):
    def _coordinator(self, root: Path, **runtime_kwargs):
        runtime = FakeRuntime(root, **runtime_kwargs)
        coordinator = YdhostLanCoordinator(runtime.dependencies(), clock=FakeClock())
        return runtime, coordinator

    def test_happy_path_requires_listen_join_and_ready_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            runtime, coordinator = self._coordinator(Path(temp))
            result = coordinator.launch(
                LanLaunchSpec("run-a", 2, timeout_seconds=1.0, poll_interval_seconds=0.1)
            )
            self.assertTrue(result["ok"], result)
            self.assertEqual("YDHOST_LAN_READY", result["code"])
            self.assertEqual(["client-01", "client-02"], [row["sessionId"] for row in result["clients"]])
            self.assertFalse(result["countsAsLanAcceptance"])
            self.assertEqual("dependency-injection-simulation", result["acceptanceScope"])
            self.assertEqual(0, runtime.host_stops)
            self.assertEqual([], runtime.stopped_clients)
            self.assertLess(runtime.calls.index("launch-host"), runtime.calls.index("launch-client-01"))

    def test_boolean_state_without_evidence_times_out_and_cleans_everything(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            runtime, coordinator = self._coordinator(Path(temp), missing_evidence=True)
            result = coordinator.launch(
                LanLaunchSpec("run-timeout", 2, timeout_seconds=0.3, poll_interval_seconds=0.1)
            )
            self.assertFalse(result["ok"])
            self.assertIn("缺少", result["error"])
            self.assertEqual(["client-02", "client-01"], runtime.stopped_clients)
            self.assertEqual(1, runtime.host_stops)
            self.assertFalse(result["countsAsLanAcceptance"])

    def test_partial_client_launch_failure_cleans_started_client_and_host(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            runtime, coordinator = self._coordinator(Path(temp), fail_client_index=2)
            result = coordinator.launch(LanLaunchSpec("run-fail", 2))
            self.assertFalse(result["ok"])
            self.assertEqual(["client-01"], runtime.stopped_clients)
            self.assertEqual(1, runtime.host_stops)

    def test_host_exit_before_listen_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            runtime, coordinator = self._coordinator(Path(temp), host_dies=True)
            result = coordinator.launch(LanLaunchSpec("run-host-dies", 2))
            self.assertFalse(result["ok"])
            self.assertIn("ydhost", result["error"])
            self.assertEqual(1, runtime.host_stops)

    def test_job_collision_is_rejected_and_cleaned(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            runtime, coordinator = self._coordinator(Path(temp), duplicate_job=True)
            result = coordinator.launch(LanLaunchSpec("run-job-collision", 2))
            self.assertFalse(result["ok"])
            self.assertIn("唯一", result["error"])
            self.assertEqual(["client-01"], runtime.stopped_clients)
            self.assertEqual(1, runtime.host_stops)

    def test_pid_collision_with_host_is_rejected_and_launched_client_is_cleaned(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            runtime, coordinator = self._coordinator(Path(temp), duplicate_pid=True)
            result = coordinator.launch(LanLaunchSpec("run-pid-collision", 2))
            self.assertFalse(result["ok"])
            self.assertIn("唯一", result["error"])
            self.assertEqual(["client-01"], runtime.stopped_clients)
            self.assertEqual(1, runtime.host_stops)

    def test_event_sink_failure_is_best_effort_and_does_not_break_cleanup(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            runtime, coordinator = self._coordinator(
                Path(temp), missing_evidence=True, raise_record_event=True
            )
            result = coordinator.launch(
                LanLaunchSpec("run-event-failure", 2, timeout_seconds=0.3, poll_interval_seconds=0.1)
            )
            self.assertFalse(result["ok"])
            self.assertEqual(["client-02", "client-01"], runtime.stopped_clients)
            self.assertEqual(1, runtime.host_stops)
            self.assertTrue(any("recordEventError" in row for row in result["events"]))

    def test_stopping_one_client_preserves_host_and_sibling(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            runtime, coordinator = self._coordinator(Path(temp))
            launch = coordinator.launch(LanLaunchSpec("run-isolation", 2))
            self.assertTrue(launch["ok"], launch)
            stopped = coordinator.stop_client("run-isolation", "client-01")
            self.assertTrue(stopped["ok"], stopped)
            self.assertTrue(stopped["hostPreserved"])
            self.assertEqual(["client-02"], stopped["remainingClients"])
            self.assertEqual(0, runtime.host_stops)
            self.assertEqual(["client-01"], runtime.stopped_clients)
            final = coordinator.stop_run("run-isolation")
            self.assertTrue(final["ok"], final)
            self.assertEqual(["client-01", "client-02"], runtime.stopped_clients)
            self.assertEqual(1, runtime.host_stops)

    def test_stop_run_retains_failed_handles_for_retry(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            runtime, coordinator = self._coordinator(
                Path(temp), fail_stop_client="client-02", fail_stop_host=True
            )
            launch = coordinator.launch(LanLaunchSpec("run-stop-errors", 2))
            self.assertTrue(launch["ok"], launch)
            stopped = coordinator.stop_run("run-stop-errors")
            self.assertFalse(stopped["ok"], stopped)
            self.assertEqual(["client-02", "client-01"], runtime.stopped_clients)
            self.assertEqual(1, runtime.host_stops)
            self.assertEqual(["client-02"], stopped["remainingClients"])
            self.assertFalse(stopped["hostStopped"])
            self.assertIsNotNone(coordinator.registry.get("run-stop-errors"))

            runtime.fail_stop_client = ""
            runtime.fail_stop_host = False
            retried = coordinator.stop_run("run-stop-errors", "retry")
            self.assertTrue(retried["ok"], retried)
            self.assertIsNone(coordinator.registry.get("run-stop-errors"))

    def test_identifiers_must_be_strict_normalized_values(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            runtime, coordinator = self._coordinator(Path(temp))
            with self.assertRaises(ValueError):
                coordinator.launch(LanLaunchSpec("../escape", 2))
            with self.assertRaises(ValueError):
                coordinator.launch(LanLaunchSpec("run-ok", 2, session_prefix="bad prefix"))
            with self.assertRaises(ValueError):
                coordinator.launch(LanLaunchSpec("run-ok", 2, session_prefix="x" * 64))
            self.assertEqual([], runtime.calls)

    def test_prepared_paths_require_sandbox_boundary_files_and_real_hashes(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            sandbox = Path(temp) / "sandbox"
            sandbox.mkdir()
            runtime = FakeRuntime(sandbox)
            spec = LanLaunchSpec("run-safe", 2)
            prepared = runtime.prepare(spec)
            YdhostLanCoordinator._validate_prepared(spec, prepared)

            escaped = PreparedLanRun(
                **{
                    **prepared.__dict__,
                    "run_root": Path(temp) / "outside",
                }
            )
            with self.assertRaisesRegex(ValueError, "边界"):
                YdhostLanCoordinator._validate_prepared(spec, escaped)

            prepared.map_cfg.unlink()
            with self.assertRaisesRegex(ValueError, "不存在"):
                YdhostLanCoordinator._validate_prepared(spec, prepared)

            prepared.map_cfg.write_bytes(b"map-config-drift")
            with self.assertRaisesRegex(ValueError, "证据不匹配"):
                YdhostLanCoordinator._validate_prepared(spec, prepared)

    def test_prepared_run_root_and_cfg_reparse_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            sandbox = Path(temp)
            runtime = FakeRuntime(sandbox)
            spec = LanLaunchSpec("run-reparse", 2)
            prepared = runtime.prepare(spec)
            real_probe = ydhost_adapter._is_reparse_point

            def run_root_reparse(path: Path) -> bool:
                return Path(path) == prepared.run_root or real_probe(path)

            with mock.patch.object(ydhost_adapter, "_is_reparse_point", side_effect=run_root_reparse):
                with self.assertRaisesRegex(ValueError, "reparse"):
                    YdhostLanCoordinator._validate_prepared(spec, prepared)

            def cfg_reparse(path: Path) -> bool:
                return Path(path) == prepared.ydhost_cfg or real_probe(path)

            with mock.patch.object(ydhost_adapter, "_is_reparse_point", side_effect=cfg_reparse):
                with self.assertRaisesRegex(ValueError, "reparse"):
                    YdhostLanCoordinator._validate_prepared(spec, prepared)

    def test_real_launcher_capability_remains_explicitly_blocked(self) -> None:
        capability = real_launch_capability()
        self.assertFalse(capability["ok"])
        self.assertEqual("YDHOST_CLIENT_LAUNCH_PROTOCOL_UNRESOLVED", capability["code"])
        self.assertFalse(capability["realProcessLauncherBound"])
        self.assertEqual(
            {
                "YDHOST_FAILED_LAUNCH_HANDLE_RETENTION_REQUIRED",
                "YDHOST_SUSPENDED_JOB_BINDING_REQUIRED",
                "MAPDUMP_SNAPSHOT_WRITE_PIN_REQUIRED",
            },
            {row["code"] for row in capability["additionalProductionBlockers"]},
        )


if __name__ == "__main__":
    unittest.main()
