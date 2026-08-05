from __future__ import annotations

import inspect
import json
import re
import threading
import unittest

import ydwe_job_owner as owned_job


class FakeBackend:
    def __init__(self) -> None:
        self.calls: list[str] = []
        self.failures: dict[str, int] = {}
        self.already_exists = False
        self.kill_on_close = True
        self.active_processes = 0
        self.job_handle = 0x1234ABCD
        self.process_handle = 0x5678EF01
        self.process_creation_time = 133700000000000000
        self.process_in_job = True
        self.process_signaled = False
        self.false_results: set[str] = set()

    def fail_once(self, operation: str) -> None:
        self.failures[operation] = self.failures.get(operation, 0) + 1

    def _call(self, operation: str) -> None:
        self.calls.append(operation)
        remaining = self.failures.get(operation, 0)
        if remaining:
            self.failures[operation] = remaining - 1
            raise OSError(5, f"synthetic {operation} failure with handle {self.job_handle}")

    def create_job(self, name: str) -> owned_job.JobCreation:
        self.created_name = name
        self._call("create_job")
        return owned_job.JobCreation(self.job_handle, self.already_exists)

    def set_kill_on_job_close(self, capability: object) -> None:
        self.assert_job(capability)
        self._call("set_kill_on_job_close")

    def query_job(self, capability: object) -> owned_job.JobSnapshot:
        self.assert_job(capability)
        self._call("query_job")
        return owned_job.JobSnapshot(self.kill_on_close, self.active_processes)

    def terminate_job(self, capability: object, exit_code: int) -> None:
        self.assert_job(capability)
        self._call("terminate_job")
        self.active_processes = 0

    def capture_root_process(self, process_id: int) -> object:
        if process_id <= 0:
            raise AssertionError("invalid pid")
        self._call("capture_root_process")
        return self.process_handle

    def query_root_process_creation_time(self, capability: object) -> int:
        if capability != self.process_handle:
            raise AssertionError("bad root capability")
        self._call("query_root_process_creation_time")
        return self.process_creation_time

    def is_process_in_job(self, process: object, job: object) -> bool:
        if process != self.process_handle:
            raise AssertionError("bad root capability")
        self.assert_job(job)
        self._call("is_process_in_job")
        return self.process_in_job

    def is_root_process_signaled(self, capability: object) -> bool:
        if capability != self.process_handle:
            raise AssertionError("bad root capability")
        self._call("is_root_process_signaled")
        return self.process_signaled

    def wait_root_process(self, capability: object, timeout_ms: int) -> bool:
        if capability != self.process_handle or timeout_ms <= 0:
            raise AssertionError("bad root capability")
        self._call("wait_root_process")
        return "wait_root_process" not in self.false_results

    def wait_zero_active_processes(self, capability: object, timeout_ms: int) -> bool:
        self.assert_job(capability)
        if timeout_ms <= 0:
            raise AssertionError("bad timeout")
        self._call("wait_zero_active_processes")
        return "wait_zero_active_processes" not in self.false_results and self.active_processes == 0

    def close_root_process(self, capability: object) -> None:
        if capability != self.process_handle:
            raise AssertionError("bad root capability")
        self._call("close_root_process")

    def close_job(self, capability: object) -> None:
        self.assert_job(capability)
        self._call("close_job")

    def assert_job(self, capability: object) -> None:
        if capability != self.job_handle:
            raise AssertionError("bad job capability")


class OwnedJobOwnerTests(unittest.TestCase):
    def create_ready(self) -> tuple[FakeBackend, owned_job.OwnedJobOwner]:
        backend = FakeBackend()
        return backend, owned_job.OwnedJobOwner.create(backend)

    def test_random_name_has_strict_local_prefix_and_is_not_reused(self) -> None:
        first_backend, first = self.create_ready()
        second_backend, second = self.create_ready()
        pattern = r"^Local\\War3AutoTest_[0-9a-f]{32}$"
        self.assertRegex(first_backend.created_name, pattern)
        self.assertRegex(second_backend.created_name, pattern)
        self.assertNotEqual(first_backend.created_name, second_backend.created_name)
        first.cleanup()
        second.cleanup()

    def test_create_sets_and_rechecks_kill_on_close_and_empty_job(self) -> None:
        backend, owner = self.create_ready()
        self.assertEqual(["create_job", "set_kill_on_job_close", "query_job"], backend.calls)
        descriptor = owner.verify_ready_for_launch()
        self.assertEqual("ready", descriptor.state)
        self.assertTrue(descriptor.killOnJobClose)
        self.assertEqual(0, descriptor.activeProcesses)
        self.assertEqual("query_job", backend.calls[-1])
        owner.cleanup()

    def test_existing_random_name_is_rejected_and_never_configured(self) -> None:
        backend = FakeBackend()
        backend.already_exists = True
        with self.assertRaises(owned_job.JobNameCollisionError) as raised:
            owned_job.OwnedJobOwner.create(backend)
        self.assertEqual(["create_job", "close_job"], backend.calls)
        self.assertEqual("closed", raised.exception.owner.descriptor().state)

    def test_existing_name_close_failure_preserves_only_rejected_reference_for_retry(self) -> None:
        backend = FakeBackend()
        backend.already_exists = True
        backend.fail_once("close_job")
        with self.assertRaises(owned_job.JobNameCollisionError) as raised:
            owned_job.OwnedJobOwner.create(backend)
        owner = raised.exception.owner
        self.assertEqual("cleanup_failed", owner.descriptor().state)
        backend.calls.clear()
        self.assertEqual("closed", owner.cleanup().state)
        self.assertEqual(["close_job"], backend.calls, "不得终止随机名称碰撞到的外部 Job")

    def test_configuration_failure_retains_owner_for_retryable_cleanup(self) -> None:
        for mode in ("set_failure", "kill_flag_missing", "nonempty"):
            with self.subTest(mode=mode):
                backend = FakeBackend()
                if mode == "set_failure":
                    backend.fail_once("set_kill_on_job_close")
                elif mode == "kill_flag_missing":
                    backend.kill_on_close = False
                else:
                    backend.active_processes = 1
                with self.assertRaises(owned_job.OwnedJobError) as raised:
                    owned_job.OwnedJobOwner.create(backend)
                owner = raised.exception.owner
                self.assertEqual("cleanup_failed", owner.descriptor().state)
                backend.kill_on_close = True
                backend.active_processes = 0
                self.assertEqual("closed", owner.cleanup().state)

    def test_raw_handles_never_appear_in_descriptor_json_or_repr(self) -> None:
        backend, owner = self.create_ready()
        backend.fail_once("terminate_job")
        owner.cleanup()
        rendered = "\n".join((
            repr(owner),
            repr(owner.descriptor()),
            owner.descriptor().to_json(),
            json.dumps(owner.descriptor().to_dict()),
        ))
        for secret in (str(backend.job_handle), hex(backend.job_handle), str(backend.process_handle)):
            self.assertNotIn(secret, rendered)
        owner.cleanup()

    def test_public_api_accepts_neither_owner_handle_nor_authorization_boolean(self) -> None:
        create_parameters = inspect.signature(owned_job.OwnedJobOwner.create).parameters
        self.assertNotIn("handle", create_parameters)
        self.assertNotIn("owner_retained", create_parameters)
        backend, owner = self.create_ready()
        with self.assertRaises(ValueError):
            owner.register_root_process(True, backend.process_creation_time)
        with self.assertRaises(ValueError):
            owner.register_root_process(4321, True)
        with self.assertRaises(ValueError):
            owner.register_root_process(0x100000000, backend.process_creation_time)
        with self.assertRaises(TypeError):
            owned_job.OwnedJobOwner.create(backend, owner_handle=backend.job_handle)  # type: ignore[call-arg]
        owner.cleanup()

    def test_normal_cleanup_order_waits_before_closing_owner(self) -> None:
        backend, owner = self.create_ready()
        backend.active_processes = 1
        owner.register_root_process(4321, backend.process_creation_time)
        backend.calls.clear()
        descriptor = owner.cleanup(timeout_ms=100)
        self.assertEqual("closed", descriptor.state)
        self.assertEqual([
            "terminate_job",
            "wait_root_process",
            "wait_zero_active_processes",
            "query_job",
            "close_root_process",
            "close_job",
        ], backend.calls)

    def test_each_cleanup_failure_retains_owner_and_retry_succeeds(self) -> None:
        operations = (
            "terminate_job", "wait_root_process", "wait_zero_active_processes",
            "query_job", "close_root_process", "close_job",
        )
        for operation in operations:
            with self.subTest(operation=operation):
                backend, owner = self.create_ready()
                backend.active_processes = 1
                owner.register_root_process(4321, backend.process_creation_time)
                backend.fail_once(operation)
                first = owner.cleanup(timeout_ms=100)
                self.assertEqual("cleanup_failed", first.state)
                self.assertTrue(first.rootProcessTracked or operation == "close_job")
                self.assertNotEqual("closed", first.state)
                second = owner.cleanup(timeout_ms=100)
                self.assertEqual("closed", second.state)
                self.assertEqual(2, second.cleanupAttempts)

    def test_wait_timeouts_also_preserve_owner_for_retry(self) -> None:
        for operation in ("wait_root_process", "wait_zero_active_processes"):
            with self.subTest(operation=operation):
                backend, owner = self.create_ready()
                backend.active_processes = 1
                owner.register_root_process(4321, backend.process_creation_time)
                backend.false_results.add(operation)
                self.assertEqual("cleanup_failed", owner.cleanup(timeout_ms=100).state)
                backend.false_results.clear()
                self.assertEqual("closed", owner.cleanup(timeout_ms=100).state)

    def test_prelaunch_recheck_failure_disables_launch_and_preserves_owner(self) -> None:
        backend, owner = self.create_ready()
        backend.active_processes = 1
        with self.assertRaises(owned_job.OwnedJobError):
            owner.verify_ready_for_launch()
        self.assertEqual("cleanup_failed", owner.descriptor().state)
        backend.active_processes = 0
        self.assertEqual("closed", owner.cleanup().state)

    def test_root_identity_requires_creation_time_membership_and_single_process(self) -> None:
        scenarios = ("creation_time", "membership", "process_exited", "multiple_processes")
        for scenario in scenarios:
            with self.subTest(scenario=scenario):
                backend, owner = self.create_ready()
                backend.active_processes = 1
                creation_time = backend.process_creation_time
                if scenario == "creation_time":
                    creation_time += 1
                elif scenario == "membership":
                    backend.process_in_job = False
                elif scenario == "process_exited":
                    backend.process_signaled = True
                else:
                    backend.active_processes = 2
                with self.assertRaises(owned_job.OwnedJobError):
                    owner.register_root_process(4321, creation_time)
                self.assertEqual("cleanup_failed", owner.descriptor().state)
                backend.process_in_job = True
                backend.process_signaled = False
                backend.active_processes = 0
                self.assertEqual("closed", owner.cleanup(timeout_ms=100).state)

    def test_root_validation_failure_never_loses_open_process_capability(self) -> None:
        backend, owner = self.create_ready()
        backend.active_processes = 1
        backend.fail_once("query_root_process_creation_time")
        with self.assertRaises(owned_job.OwnedJobError):
            owner.register_root_process(4321, backend.process_creation_time)
        failed = owner.descriptor()
        self.assertTrue(failed.rootProcessTracked)
        self.assertTrue(failed.rootProcessInJob)
        self.assertEqual("closed", owner.cleanup(timeout_ms=100).state)

        backend, owner = self.create_ready()
        backend.active_processes = 1
        backend.process_in_job = False
        backend.fail_once("close_root_process")
        with self.assertRaises(owned_job.OwnedJobError):
            owner.register_root_process(4321, backend.process_creation_time)
        failed = owner.descriptor()
        self.assertTrue(failed.rootProcessTracked)
        self.assertFalse(failed.rootProcessInJob)
        backend.calls.clear()
        self.assertEqual("closed", owner.cleanup(timeout_ms=100).state)
        self.assertNotIn("wait_root_process", backend.calls,
                         "不得等待或误杀未证明属于 owner Job 的 PID")

    def test_concurrent_cleanup_closes_each_capability_exactly_once(self) -> None:
        backend, owner = self.create_ready()
        backend.active_processes = 1
        owner.register_root_process(4321, backend.process_creation_time)
        entered = threading.Event()
        release = threading.Event()
        original_terminate = backend.terminate_job

        def blocking_terminate(capability: object, exit_code: int) -> None:
            entered.set()
            self.assertTrue(release.wait(2.0))
            original_terminate(capability, exit_code)

        backend.terminate_job = blocking_terminate  # type: ignore[method-assign]
        results: list[owned_job.OwnedJobDescriptor] = []
        first = threading.Thread(target=lambda: results.append(owner.cleanup(timeout_ms=100)))
        second = threading.Thread(target=lambda: results.append(owner.cleanup(timeout_ms=100)))
        first.start()
        self.assertTrue(entered.wait(1.0))
        second.start()
        self.assertTrue(second.is_alive(), "第二个 cleanup 必须等待 owner lock")
        release.set()
        first.join(2.0)
        second.join(2.0)
        self.assertFalse(first.is_alive())
        self.assertFalse(second.is_alive())
        self.assertEqual(["closed", "closed"], sorted(row.state for row in results))
        self.assertEqual(1, backend.calls.count("close_root_process"))
        self.assertEqual(1, backend.calls.count("close_job"))

    def test_concurrent_register_cannot_overwrite_process_capability(self) -> None:
        backend, owner = self.create_ready()
        backend.active_processes = 1
        entered = threading.Event()
        release = threading.Event()
        original_capture = backend.capture_root_process

        def blocking_capture(process_id: int) -> object:
            entered.set()
            self.assertTrue(release.wait(2.0))
            return original_capture(process_id)

        backend.capture_root_process = blocking_capture  # type: ignore[method-assign]
        errors: list[BaseException] = []

        def register() -> None:
            try:
                owner.register_root_process(4321, backend.process_creation_time)
            except BaseException as error:
                errors.append(error)

        first = threading.Thread(target=register)
        second = threading.Thread(target=register)
        first.start()
        self.assertTrue(entered.wait(1.0))
        second.start()
        self.assertTrue(second.is_alive(), "第二个 register 必须等待 owner lock")
        release.set()
        first.join(2.0)
        second.join(2.0)
        self.assertEqual(1, backend.calls.count("capture_root_process"))
        self.assertEqual(1, len(errors))
        self.assertIsInstance(errors[0], owned_job.OwnedJobError)
        self.assertEqual("active", owner.descriptor().state)
        self.assertEqual("closed", owner.cleanup(timeout_ms=100).state)


if __name__ == "__main__":
    unittest.main()
