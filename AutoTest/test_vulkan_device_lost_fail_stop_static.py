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
QUEUE = (ROOT / "src/dxvk/dxvk_queue.cpp").read_text(encoding="utf-8")
PRESENTER = (ROOT / "src/dxvk/dxvk_presenter.cpp").read_text(
    encoding="utf-8"
)
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

    def test_lost_cs_sync_waits_only_for_an_emitted_sequence(self):
        sync = function_body(DEVICE_CPP, "D3D9DeviceEx::SynchronizeCsThread")
        lost_gate = sync.index("if (unlikely(IsVulkanDeviceLostFailStop()))")
        normal_flush = sync.index("if (SequenceNumber > m_csSeqNum)")
        lost_path = sync[lost_gate:normal_flush]

        self.assertIn("FlushCsChunk();", lost_path)
        self.assertIn("m_csThread.synchronize(m_csSeqNum);", lost_path)
        self.assertIn("return;", lost_path)
        self.assertNotIn("m_csThread.synchronize(SequenceNumber);", lost_path)

    def test_terminal_submission_entries_still_finish_on_the_cpu(self):
        submit = function_body(QUEUE, "DxvkSubmissionQueue::submitCmdLists")
        finish = function_body(QUEUE, "DxvkSubmissionQueue::finishCmdLists")
        queue_error = function_body(QUEUE, "DxvkSubmissionQueue::setQueueError")

        first_submit = submit.index("entry.submit.cmdList->submit")
        first_terminal_gate = submit.index(
            "m_device->getDeviceStatus() != VK_ERROR_DEVICE_LOST"
        )
        self.assertLess(first_terminal_gate, first_submit)
        self.assertIn("retireTerminalFrame", submit)
        self.assertIn("m_finishQueue.push(std::move(entry));", submit)
        self.assertIn("m_submitCond.notify_all();", submit)

        first_timeline_wait = finish.index("vk->vkWaitSemaphores")
        self.assertLess(
            finish.index("m_device->getDeviceStatus() == VK_ERROR_DEVICE_LOST"),
            first_timeline_wait,
        )
        self.assertIn("status == VK_SUCCESS", finish[:first_timeline_wait])
        for token in (
            "entry.submit.cmdList->notifyObjects();",
            "entry.submit.cmdList->reset();",
            "m_device->recycleCommandList(entry.submit.cmdList);",
            "m_finishCond.notify_all();",
        ):
            self.assertIn(token, finish)

        self.assertIn("m_lastError.store(status);", queue_error)
        self.assertIn("current != VK_ERROR_DEVICE_LOST", queue_error)

    def test_terminal_present_frames_skip_wsi_waits_but_signal(self):
        retire = function_body(PRESENTER, "Presenter::retireTerminalFrame")
        frame_thread = function_body(PRESENTER, "Presenter::runFrameThread")
        fence_wait = function_body(PRESENTER, "Presenter::waitForSwapchainFence")
        signal = function_body(PRESENTER, "Presenter::signalFrame")

        self.assertIn("frame.result = VK_ERROR_DEVICE_LOST;", retire)
        self.assertIn("m_presentPending = false;", retire)
        self.assertIn("m_frameCond.notify_one();", retire)
        self.assertNotIn("m_vkd->", retire)

        first_present_wait = frame_thread.index("vkWaitForPresent")
        self.assertLess(
            frame_thread.index("m_device->getDeviceStatus() == VK_ERROR_DEVICE_LOST"),
            first_present_wait,
        )
        self.assertIn("if (!terminal)", frame_thread)
        self.assertIn("m_frameDrain.notify_one();", frame_thread)
        self.assertIn("m_lastCompleted = frame.frameId;", frame_thread)

        self.assertLess(
            fence_wait.index("m_device->getDeviceStatus() == VK_ERROR_DEVICE_LOST"),
            fence_wait.index("vkWaitForFences"),
        )
        self.assertIn("if (!terminal)", signal)

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
