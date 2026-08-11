#!/usr/bin/env python3
"""Contracts for the development-only device-address binding fault correlator."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


class DeviceAddressBindingReportStaticTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.options = read("meson_options.txt")
        cls.meson = read("src/dxvk/meson.build")
        cls.d3d9_meson = read("src/d3d9/meson.build")
        cls.info_h = read("src/dxvk/dxvk_device_info.h")
        cls.info_cpp = read("src/dxvk/dxvk_device_info.cpp")
        cls.adapter = read("src/dxvk/dxvk_adapter.cpp")
        cls.instance = read("src/dxvk/dxvk_instance.cpp")
        cls.tracker_h = read("src/dxvk/dxvk_device_address_binding.h")
        cls.tracker_cpp = read("src/dxvk/dxvk_device_address_binding.cpp")
        cls.fault_cpp = read("src/dxvk/dxvk_device_fault.cpp")
        cls.diagnostics = read(
            "src/d3d9/war3/tools/war3_diagnostics_hub.cpp"
        )

    def test_build_policy_is_default_off_and_compile_time_only(self) -> None:
        option_at = self.options.index("warvk_device_address_binding_report_dev")
        self.assertIn("value : false", self.options[option_at:option_at + 240])
        self.assertIn(
            "WARVK_ENABLE_DEVICE_ADDRESS_BINDING_REPORT_DEV=1", self.meson
        )
        self.assertIn(
            "WARVK_ENABLE_DEVICE_ADDRESS_BINDING_REPORT_DEV=1",
            self.d3d9_meson,
        )
        self.assertNotIn("getEnvVar", self.tracker_h + self.tracker_cpp)
        self.assertIn("DxvkDeviceAddressBindingBuildEnabled = false", self.tracker_h)

    def test_extension_feature_is_dev_guarded(self) -> None:
        self.assertIn("extDeviceAddressBindingReport", self.info_h)
        self.assertIn("HANDLE_EXT(extDeviceAddressBindingReport)", self.info_cpp)
        feature = "ENABLE_EXT_FEATURE(extDeviceAddressBindingReport, reportAddressBinding, false)"
        feature_at = self.info_cpp.index(feature)
        guard_at = self.info_cpp.rfind(
            "#if defined(WARVK_ENABLE_DEVICE_ADDRESS_BINDING_REPORT_DEV)",
            0,
            feature_at,
        )
        end_at = self.info_cpp.index("#endif", feature_at)
        self.assertGreaterEqual(guard_at, 0)
        self.assertLess(feature_at, end_at)
        self.assertIn(
            "!instance.extensions().extDebugUtils.specVersion", self.info_cpp
        )
        self.assertIn(
            "extDeviceAddressBindingReport.reportAddressBinding = VK_FALSE",
            self.info_cpp,
        )

    def test_tracker_is_active_during_device_creation(self) -> None:
        create_at = self.adapter.index("vk->vkCreateDevice")
        enable_at = self.adapter.rfind(
            "GetDxvkDeviceAddressBindingTracker().setDeviceFeatureEnabled(",
            0,
            create_at,
        )
        self.assertGreaterEqual(enable_at, 0)
        failure_at = self.adapter.index("if (vr)", create_at)
        failure = self.adapter[failure_at:self.adapter.index(
            "Rc<vk::DeviceFn>", failure_at
        )]
        self.assertIn("setDeviceFeatureEnabled(false)", failure)

    def test_driver_callback_is_bounded_and_non_blocking(self) -> None:
        callback_at = self.instance.index("DxvkInstance::addressBindingCallback")
        callback = self.instance[callback_at:]
        self.assertIn(
            "VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT",
            callback,
        )
        self.assertIn("VkDeviceAddressBindingCallbackDataEXT", callback)
        self.assertIn("->record(", callback)
        record_at = self.tracker_cpp.index(
            "DxvkDeviceAddressBindingTracker::record"
        )
        record_end = self.tracker_cpp.index(
            "DxvkDeviceAddressBindingTracker::readSlot", record_at
        )
        record = self.tracker_cpp[record_at:record_end]
        for forbidden in (
            "new ", "std::vector", "std::string", "mutex", "condition_variable",
            "Logger", "sleep", "wait", "vkGetDeviceFaultInfo",
        ):
            self.assertNotIn(forbidden, record)
        self.assertIn(
            "DxvkDeviceAddressBindingBuildEnabled ? 16384u : 1u",
            self.tracker_h,
        )
        self.assertIn("MaxMatches = 32u", self.tracker_h)
        self.assertIn("ObjectNameCapacity = 96u", self.tracker_h)
        self.assertIn("std::array<std::atomic<uint32_t>", self.tracker_h)
        self.assertIn("object->pObjectName", record)

    def test_fault_correlation_uses_khronos_precision_contract(self) -> None:
        self.assertIn("address.reportedAddress & ~mask", self.tracker_cpp)
        self.assertIn("address.reportedAddress | mask", self.tracker_cpp)
        self.assertIn("SaturatingRangeEnd", self.tracker_cpp)
        self.assertIn("m_addressBinding = GetDxvkDeviceAddressBindingTracker().correlate", self.fault_cpp)
        self.assertIn('"addressBindingReport"', self.diagnostics)
        for field in (
            '"observedEventCount"', '"droppedEventCount"', '"matches"',
            '"baseAddress"', '"objectHandle"',
            '"objectName"', '"latestState"', '"previousBindingType"',
            '"priorBindSequence"',
        ):
            self.assertIn(field, self.diagnostics)

    def test_correlation_reports_latest_exact_lifecycle(self) -> None:
        self.assertIn("SameObjectRange", self.tracker_cpp)
        self.assertIn("latestForObjectRange = true", self.tracker_cpp)
        self.assertIn("previous.sequence > match.previousSequence", self.tracker_cpp)
        self.assertIn("previous.sequence > match.priorBindSequence", self.tracker_cpp)
        self.assertIn("VK_DEVICE_ADDRESS_BINDING_TYPE_BIND_EXT", self.tracker_cpp)

    def test_callback_path_does_not_write_incidents_or_query_faults(self) -> None:
        combined = self.instance + self.tracker_cpp
        self.assertNotIn("WriteGpuIncident", combined)
        self.assertNotIn("NotifyGpuDeviceLost", combined)
        self.assertNotIn("captureDeviceFault", combined)
        self.assertNotIn("vkGetDeviceFaultInfoEXT", combined)


if __name__ == "__main__":
    unittest.main()
