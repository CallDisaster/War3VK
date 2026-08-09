#!/usr/bin/env python3
"""P5c contracts for owner-safe fence and keyed-mutex loss provenance."""

from pathlib import Path
import unittest
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[1]
REGISTRY = ROOT / "include/vulkan/registry/vk.xml"
LOADER = (ROOT / "src/vulkan/vulkan_loader.h").read_text(encoding="utf-8")
DEVICE = (ROOT / "src/dxvk/dxvk_device.h").read_text(encoding="utf-8")
FENCE = (ROOT / "src/dxvk/dxvk_fence.cpp").read_text(encoding="utf-8")
FENCE_H = (ROOT / "src/dxvk/dxvk_fence.h").read_text(encoding="utf-8")
IMAGE = (ROOT / "src/dxvk/dxvk_image.cpp").read_text(encoding="utf-8")
IMAGE_H = (ROOT / "src/dxvk/dxvk_image.h").read_text(encoding="utf-8")


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


def class_body(source: str, signature: str) -> str:
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
    raise AssertionError(f"unterminated class: {signature}")


def error_codes() -> dict[str, set[str]]:
    root = ET.parse(REGISTRY).getroot()
    result: dict[str, set[str]] = {}
    for command in root.findall("./commands/command"):
        name = command.findtext("./proto/name")
        if name:
            result[name] = set(command.get("errorcodes", "").split(","))
    return result


class VulkanDeviceLostFenceStateStaticTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.error_codes = error_codes()

    def test_registry_confines_p5c_to_wait_and_counter_queries(self):
        for command in ("vkWaitSemaphores", "vkGetSemaphoreCounterValue"):
            self.assertIn("VK_ERROR_DEVICE_LOST", self.error_codes[command])

        for command in (
            "vkCreateSemaphore",
            "vkImportSemaphoreWin32HandleKHR",
            "vkGetSemaphoreWin32HandleKHR",
            "vkSignalSemaphore",
        ):
            self.assertNotIn("VK_ERROR_DEVICE_LOST", self.error_codes[command])

    def test_device_fn_owns_the_only_pure_direct_loss_state(self):
        state = function_body(LOADER, "struct DeviceLossState")
        self.assertIn("std::atomic<bool> m_driverDeviceLossObserved", state)
        self.assertIn("status == VK_ERROR_DEVICE_LOST", state)
        self.assertIn("std::memory_order_release", state)
        self.assertIn("std::memory_order_acquire", state)
        for forbidden in (
            "Logger",
            "new ",
            "mutex",
            "DxvkDevice*",
            "Rc<DxvkDevice>",
            "captureDeviceFault",
            "vkGetDeviceFaultInfoEXT",
        ):
            self.assertNotIn(forbidden, state)

        device_fn = LOADER[LOADER.index("struct DeviceFn") :]
        self.assertIn("DeviceLossState m_deviceLossState", device_fn)
        self.assertIn("m_deviceLossState.notifyDeviceErrorFromDriverResult", device_fn)
        self.assertIn("m_deviceLossState.driverDeviceLossObserved", device_fn)
        self.assertNotIn("m_driverDeviceLossObserved = { false }", DEVICE)

    def test_dxvk_device_reads_the_shared_state_for_terminal_and_capture_policy(self):
        status = function_body(DEVICE, "VkResult getDeviceStatus")
        self.assertLess(
            status.index("m_vkd->driverDeviceLossObserved()"),
            status.index("m_terminalStatus.load"),
        )
        notifier = function_body(DEVICE, "void notifyDeviceErrorFromDriverResult")
        self.assertLess(
            notifier.index("m_vkd->notifyDeviceErrorFromDriverResult(status);"),
            notifier.index("notifyDeviceError(status);"),
        )
        capture = function_body(
            DEVICE, "void captureDeviceFaultIfDriverLossObserved"
        )
        self.assertIn("m_vkd->driverDeviceLossObserved()", capture)
        self.assertNotIn("m_driverDeviceLossObserved", DEVICE)

    def test_fence_direct_results_are_reported_before_existing_failure_paths(self):
        wait = function_body(FENCE, "DxvkFence::wait")
        self.assertLess(
            wait.index("m_vkd->vkWaitSemaphores"),
            wait.index("m_vkd->notifyDeviceErrorFromDriverResult(vr);"),
        )
        self.assertLess(
            wait.index("m_vkd->notifyDeviceErrorFromDriverResult(vr);"),
            wait.index("Logger::err"),
        )

        run = function_body(FENCE, "DxvkFence::run")
        counter = run.index("m_vkd->vkGetSemaphoreCounterValue")
        counter_notify = run.index(
            "m_vkd->notifyDeviceErrorFromDriverResult(vr);", counter
        )
        counter_log = run.index("Logger::err", counter_notify)
        wait_call = run.index("m_vkd->vkWaitSemaphores", counter_log)
        wait_notify = run.index(
            "m_vkd->notifyDeviceErrorFromDriverResult(vr);", wait_call
        )
        wait_log = run.index("Logger::err", wait_notify)
        self.assertLess(counter, counter_notify)
        self.assertLess(counter_notify, counter_log)
        self.assertLess(wait_call, wait_notify)
        self.assertLess(wait_notify, wait_log)

        value = function_body(FENCE, "DxvkFence::getValue")
        self.assertLess(
            value.index("m_vkd->vkGetSemaphoreCounterValue"),
            value.index("m_vkd->notifyDeviceErrorFromDriverResult(vr);"),
        )
        self.assertLess(
            value.index("m_vkd->notifyDeviceErrorFromDriverResult(vr);"),
            value.index("Logger::err"),
        )

    def test_keyed_mutex_uses_its_existing_device_fn_owner_and_no_fault_capture(self):
        acquire = function_body(IMAGE, "DxvkKeyedMutex::AcquireSync")
        wait = acquire.index("VkResult vr = m_vkd->vkWaitSemaphores")
        notify = acquire.index("m_vkd->notifyDeviceErrorFromDriverResult(vr);")
        failure = acquire.index("if (vr)")
        self.assertLess(wait, notify)
        self.assertLess(notify, failure)

        for source in (FENCE, IMAGE):
            for forbidden in (
                "captureDeviceFaultIfDriverLossObserved",
                "vkGetDeviceFaultInfoEXT",
                "NotifyGpuDeviceLostFailStop",
            ):
                self.assertNotIn(forbidden, source)

        fence_private = class_body(FENCE_H, "class DxvkFence : public RcObject")
        fence_private = fence_private[fence_private.index("private:") :]
        image_private = class_body(IMAGE_H, "class DxvkKeyedMutex : public RcObject")
        image_private = image_private[image_private.index("private:") :]
        self.assertIn("Rc<vk::DeviceFn>", fence_private)
        self.assertIn("m_vkd", fence_private)
        self.assertIn("Rc<vk::DeviceFn>", image_private)
        self.assertIn("m_vkd", image_private)
        self.assertNotIn("Rc<DxvkDevice>", fence_private)
        self.assertNotIn("Rc<DxvkDevice>", image_private)
        self.assertNotIn("DxvkDevice*", fence_private)
        self.assertNotIn("DxvkDevice*", image_private)


if __name__ == "__main__":
    unittest.main()
