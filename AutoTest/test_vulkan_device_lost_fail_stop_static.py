#!/usr/bin/env python3
"""Static contracts for irreversible Vulkan device-loss fail-stop."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
DEVICE_H = (ROOT / "src/d3d9/d3d9_device.h").read_text(encoding="utf-8")
DEVICE_CPP = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")
SWAPCHAIN = (ROOT / "src/d3d9/d3d9_swapchain.cpp").read_text(encoding="utf-8")
PIPELINE = (ROOT / "src/d3d9/d3d9_war3_pipeline.cpp").read_text(
    encoding="utf-8"
)
CS_THREAD = (ROOT / "src/dxvk/dxvk_cs.cpp").read_text(encoding="utf-8")
DIAGNOSTICS = (
    ROOT / "src/d3d9/war3/tools/war3_diagnostics_hub.cpp"
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


class VulkanDeviceLostFailStopStaticTests(unittest.TestCase):
    def test_queue_error_is_an_irreversible_lock_free_gate(self):
        self.assertIn("m_vkDeviceLostFailStop", DEVICE_H)
        self.assertIn("getDeviceStatus() == VK_ERROR_DEVICE_LOST", DEVICE_H)
        self.assertNotIn("m_vkDeviceLostFailStop.store(false", DEVICE_CPP)
        latch = function_body(
            DEVICE_CPP, "D3D9DeviceEx::CheckVulkanDeviceLostFailStop"
        )
        self.assertIn("compare_exchange_strong", latch)
        self.assertIn("NotifyGpuDeviceLostFailStop", latch)
        self.assertIn("m_war3ShadowSessionReady.store(false", latch)

    def test_present_never_masks_device_loss_with_gdi_success(self):
        present = function_body(SWAPCHAIN, "D3D9SwapChainEx::Present(")
        self.assertIn("D3D9SwapChain.Present.Entry", present)
        self.assertIn("D3D9SwapChain.Present.Exception", present)
        exception_gate = present.index("D3D9SwapChain.Present.Exception")
        gdi_after_exception = present.index("return PresentImageGDI", exception_gate)
        self.assertLess(exception_gate, gdi_after_exception)

        gdi = function_body(SWAPCHAIN, "D3D9SwapChainEx::PresentImageGDI")
        self.assertLess(
            gdi.index("CheckVulkanDeviceLostFailStop"),
            gdi.index("m_parent->EndFrame"),
        )
        self.assertIn("return D3DERR_DEVICEREMOVED", gdi)

    def test_reset_and_cooperative_level_cannot_reopen_device(self):
        reset = function_body(DEVICE_CPP, "D3D9DeviceEx::Reset(")
        self.assertLess(
            reset.index("CheckVulkanDeviceLostFailStop"),
            reset.index("m_deviceLostState = D3D9DeviceLostState::Ok"),
        )
        cooperative = function_body(
            DEVICE_CPP, "D3D9DeviceEx::TestCooperativeLevel"
        )
        self.assertIn("D3DERR_DEVICEREMOVED", cooperative)

    def test_command_stream_is_dropped_and_sequences_still_drain(self):
        self.assertRegex(
            DEVICE_H,
            re.compile(
                r"template <bool AllowFlush.*?EmitCs\(.*?"
                r"IsVulkanDeviceLostFailStop",
                re.DOTALL,
            ),
        )
        self.assertIn(
            "m_device->getDeviceStatus() != VK_ERROR_DEVICE_LOST", CS_THREAD
        )
        self.assertIn("if (entry.seq)", CS_THREAD)

    def test_front_end_draws_resources_and_war3_passes_fail_closed(self):
        for origin in (
            "CreateTexture",
            "CreateVolumeTexture",
            "CreateCubeTexture",
            "CreateVertexBuffer",
            "CreateIndexBuffer",
            "CreateRenderTargetEx",
            "CreateOffscreenPlainSurfaceEx",
            "CreateDepthStencilSurfaceEx",
            "DrawPrimitive",
            "DrawIndexedPrimitive",
            "DrawPrimitiveUP",
            "DrawIndexedPrimitiveUP",
        ):
            self.assertIn(f'"D3D9Device.{origin}"', DEVICE_CPP)
        execute = function_body(PIPELINE, "War3RenderPipeline::Execute")
        self.assertIn("getDeviceStatus() == VK_ERROR_DEVICE_LOST", execute)
        self.assertLess(
            execute.index("getDeviceStatus() == VK_ERROR_DEVICE_LOST"),
            execute.index("for (auto& entry : m_passes)"),
        )

    def test_first_error_incident_uses_atomic_breadcrumbs_only(self):
        notify = function_body(
            DIAGNOSTICS, "NotifyGpuDeviceLostFailStop(const char* origin)"
        )
        self.assertIn("s_gpuDeviceLostIncidentLatched = true", notify)
        self.assertNotIn("s_gpuIncidentLatched = true", notify)
        self.assertIn("queue-error-device-lost-fail-stop", notify)
        self.assertIn("s_gpuFlightBreadcrumb.load", notify)
        self.assertNotIn("RunWithActiveDevice", notify)
        self.assertIn('"firstErrorOrigin"', DIAGNOSTICS)


if __name__ == "__main__":
    unittest.main()
