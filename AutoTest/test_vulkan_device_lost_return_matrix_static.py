#!/usr/bin/env python3
"""P5b contracts for owner-safe Vulkan device-lost return paths."""

from pathlib import Path
import unittest
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[1]
REGISTRY = ROOT / "include/vulkan/registry/vk.xml"
DEVICE = (ROOT / "src/dxvk/dxvk_device.cpp").read_text(encoding="utf-8")
PRESENTER = (ROOT / "src/dxvk/dxvk_presenter.cpp").read_text(encoding="utf-8")
PERF_MONITOR = (
    ROOT / "src/d3d9/war3/tools/war3_perf_monitor.cpp"
).read_text(encoding="utf-8")


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[brace + 1 : index]
    raise AssertionError(f"unterminated function: {signature}")


def error_codes() -> dict[str, set[str]]:
    root = ET.parse(REGISTRY).getroot()
    result: dict[str, set[str]] = {}
    for command in root.findall("./commands/command"):
        name = command.findtext("./proto/name")
        if name:
            result[name] = set(command.get("errorcodes", "").split(","))
    return result


class VulkanDeviceLostReturnMatrixStaticTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.error_codes = error_codes()

    def test_registry_limits_p5b_to_commands_permitted_to_return_device_lost(self):
        for command in (
            "vkDeviceWaitIdle",
            "vkWaitSemaphores",
            "vkGetQueryPoolResults",
        ):
            self.assertIn("VK_ERROR_DEVICE_LOST", self.error_codes[command])

        for command in (
            "vkCreateDescriptorPool",
            "vkAllocateDescriptorSets",
            "vkResetDescriptorPool",
            "vkCreateDescriptorSetLayout",
            "vkCreateDescriptorUpdateTemplate",
            "vkCreatePipelineLayout",
            "vkCreateSampler",
        ):
            self.assertNotIn("VK_ERROR_DEVICE_LOST", self.error_codes[command])

    def test_wait_for_idle_reports_direct_result_before_existing_log_and_unlock(self):
        body = function_body(DEVICE, "DxvkDevice::waitForIdle")
        wait = body.index("VkResult vr = m_vkd->vkDeviceWaitIdle")
        notify = body.index("notifyDeviceErrorFromDriverResult(vr);")
        log = body.index("Logger::err(\"DxvkDevice: waitForIdle: Operation failed\")")
        unlock = body.index("m_submissionQueue.unlockDeviceQueue();")
        self.assertLess(wait, notify)
        self.assertLess(notify, log)
        self.assertLess(log, unlock)

    def test_latency_sleep_reports_direct_wait_result_without_changing_timing_order(self):
        body = function_body(PRESENTER, "Presenter::latencySleepNv")
        wait = body.index("VkResult vr = m_vkd->vkWaitSemaphores")
        notify = body.index("m_device->notifyDeviceErrorFromDriverResult(vr);")
        elapsed = body.index("auto t1 = dxvk::high_resolution_clock::now();")
        self.assertLess(wait, notify)
        self.assertLess(notify, elapsed)
        self.assertLess(body.index("lock.unlock();"), wait)

    def test_perf_tick_drops_begin_failure_before_end_query_and_reports_end_failure(self):
        body = function_body(PERF_MONITOR, "War3PerfMonitor::tick")
        begin_query = body.index("VkResult beginRes = vk->vkGetQueryPoolResults")
        begin_pending = body.index("if (beginRes == VK_NOT_READY)")
        begin_failure = body.index("if (beginRes != VK_SUCCESS)")
        end_query = body.index("VkResult endRes = vk->vkGetQueryPoolResults")
        self.assertLess(begin_query, begin_pending)
        self.assertLess(begin_pending, begin_failure)
        self.assertLess(begin_failure, end_query)

        begin_notify = body.index(
            "m_device->notifyDeviceErrorFromDriverResult(beginRes);",
            begin_failure,
        )
        begin_drop = body.index("dropCurrentSample();", begin_notify)
        begin_continue = body.index("continue;", begin_drop)
        self.assertLess(begin_failure, begin_notify)
        self.assertLess(begin_notify, begin_drop)
        self.assertLess(begin_drop, begin_continue)
        self.assertLess(begin_continue, end_query)

        end_pending = body.index("if (endRes == VK_NOT_READY)")
        end_failure = body.index("if (endRes != VK_SUCCESS)")
        end_notify = body.index(
            "m_device->notifyDeviceErrorFromDriverResult(endRes);", end_failure
        )
        end_drop = body.index("dropCurrentSample();", end_notify)
        self.assertLess(end_query, end_pending)
        self.assertLess(end_pending, end_failure)
        self.assertLess(end_failure, end_notify)
        self.assertLess(end_notify, end_drop)
        self.assertLess(end_drop, body.index("if (endTs > beginTs"))

    def test_p5b_functions_do_not_capture_or_write_device_fault_incidents(self):
        for body in (
            function_body(DEVICE, "DxvkDevice::waitForIdle"),
            function_body(PRESENTER, "Presenter::latencySleepNv"),
            function_body(PERF_MONITOR, "War3PerfMonitor::tick"),
        ):
            self.assertNotIn("captureDeviceFaultIfDriverLossObserved", body)
            self.assertNotIn("vkGetDeviceFaultInfoEXT", body)
            self.assertNotIn("NotifyGpuDeviceLostFailStop", body)


if __name__ == "__main__":
    unittest.main()
