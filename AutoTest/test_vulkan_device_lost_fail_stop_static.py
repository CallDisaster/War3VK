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
DXVK_DEVICE_CPP = (ROOT / "src/dxvk/dxvk_device.cpp").read_text(
    encoding="utf-8"
)
DXVK_DEVICE_H = (ROOT / "src/dxvk/dxvk_device.h").read_text(
    encoding="utf-8"
)
DXVK_DEVICE_INFO_CPP = (ROOT / "src/dxvk/dxvk_device_info.cpp").read_text(
    encoding="utf-8"
)
DXVK_DEVICE_INFO_H = (ROOT / "src/dxvk/dxvk_device_info.h").read_text(
    encoding="utf-8"
)
DXVK_DEVICE_FAULT_H = (ROOT / "src/dxvk/dxvk_device_fault.h").read_text(
    encoding="utf-8"
)
DXVK_DEVICE_FAULT_CPP = (ROOT / "src/dxvk/dxvk_device_fault.cpp").read_text(
    encoding="utf-8"
)
VULKAN_LOADER = (ROOT / "src/vulkan/vulkan_loader.h").read_text(
    encoding="utf-8"
)
PIPELINE_MANAGER = (ROOT / "src/dxvk/dxvk_pipemanager.cpp").read_text(
    encoding="utf-8"
)
QUEUE = (ROOT / "src/dxvk/dxvk_queue.cpp").read_text(encoding="utf-8")
PRESENTER = (ROOT / "src/dxvk/dxvk_presenter.cpp").read_text(
    encoding="utf-8"
)
MEMORY = (ROOT / "src/dxvk/dxvk_memory.cpp").read_text(encoding="utf-8")
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

    def test_d3d9ex_reset_and_additional_swapchain_fail_stop(self):
        reset_ex = function_body(DEVICE_CPP, "D3D9DeviceEx::ResetEx(")
        reset_gate = reset_ex.index(
            'CheckVulkanDeviceLostFailStop("D3D9Device.ResetEx")'
        )
        validate = reset_ex.index("ValidatePresentationParametersEx")
        reset_swapchain = reset_ex.index("ResetSwapChain")
        reset_gpu_skin = reset_ex.index("War3ResetGpuSkinDeviceEpoch")

        self.assertLess(reset_ex.index("D3D9DeviceLock lock = LockDevice();"), reset_gate)
        self.assertLess(reset_gate, validate)
        self.assertLess(reset_gate, reset_swapchain)
        self.assertLess(reset_gate, reset_gpu_skin)
        terminal_reset = reset_ex[reset_gate:validate]
        self.assertIn("return D3DERR_DEVICEREMOVED;", terminal_reset)

        additional = function_body(
            DEVICE_CPP, "D3D9DeviceEx::CreateAdditionalSwapChainEx"
        )
        init = additional.index("InitReturnPtr(ppSwapChain);")
        null_check = additional.index(
            "ppSwapChain == nullptr || pPresentationParameters == nullptr"
        )
        additional_gate = additional.index(
            '"D3D9Device.CreateAdditionalSwapChainEx"'
        )
        window_check = additional.index("if (!pPresentationParameters->Windowed)")
        invalidate = additional.index("m_implicitSwapchain->Invalidate")
        allocate = additional.index("new D3D9SwapChainEx")

        self.assertLess(init, null_check)
        self.assertLess(null_check, additional_gate)
        self.assertLess(additional_gate, window_check)
        self.assertLess(additional_gate, invalidate)
        self.assertLess(additional_gate, allocate)
        terminal_additional = additional[additional_gate:window_check]
        self.assertIn("return D3DERR_DEVICEREMOVED;", terminal_additional)
        self.assertNotIn("*ppSwapChain =", terminal_additional)
        self.assertIn("if (unlikely(IsDeviceLost()))", additional)

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

    def test_terminal_latch_blocks_new_shader_compiler_work(self):
        vertex = function_body(DEVICE_CPP, "D3D9DeviceEx::CreateVertexShader")
        pixel = function_body(DEVICE_CPP, "D3D9DeviceEx::CreatePixelShader")

        vertex_gate = vertex.index(
            'CheckVulkanDeviceLostFailStop("D3D9Device.CreateVertexShader")'
        )
        vertex_module = vertex.index("CreateShaderModule")
        self.assertLess(vertex.index("ppShader == nullptr"), vertex_gate)
        self.assertLess(vertex_gate, vertex_module)
        self.assertLess(vertex_gate, vertex.index("new D3D9VertexShader"))
        terminal_vertex = vertex[vertex_gate:vertex_module]
        self.assertIn("return D3DERR_DEVICEREMOVED;", terminal_vertex)
        self.assertNotIn("*ppShader", terminal_vertex)

        pixel_gate = pixel.index(
            'CheckVulkanDeviceLostFailStop("D3D9Device.CreatePixelShader")'
        )
        pixel_module = pixel.index("CreateShaderModule")
        self.assertLess(pixel.index("InitReturnPtr(ppShader);"), pixel_gate)
        self.assertLess(pixel.index("ppShader == nullptr"), pixel_gate)
        self.assertLess(pixel_gate, pixel_module)
        self.assertLess(pixel_gate, pixel.index("new D3D9PixelShader"))
        terminal_pixel = pixel[pixel_gate:pixel_module]
        self.assertIn("return D3DERR_DEVICEREMOVED;", terminal_pixel)
        self.assertNotIn("*ppShader =", terminal_pixel)

        register = function_body(DXVK_DEVICE_CPP, "DxvkDevice::registerShader")
        request = function_body(DXVK_DEVICE_CPP, "DxvkDevice::requestCompileShader")
        for wrapper, dispatch in (
            (register, "m_objects.pipelineManager().registerShader(shader);"),
            (request, "m_objects.pipelineManager().requestCompileShader(shader);"),
        ):
            self.assertLess(
                wrapper.index("getDeviceStatus() == VK_ERROR_DEVICE_LOST"),
                wrapper.index(dispatch),
            )

        library = function_body(
            PIPELINE_MANAGER, "DxvkPipelineWorkers::compilePipelineLibrary"
        )
        graphics = function_body(
            PIPELINE_MANAGER, "DxvkPipelineWorkers::compileGraphicsPipeline"
        )
        worker = function_body(PIPELINE_MANAGER, "DxvkPipelineWorkers::runWorker")

        for enqueue in (library, graphics):
            terminal_gate = enqueue.index(
                "m_device->getDeviceStatus() == VK_ERROR_DEVICE_LOST"
            )
            self.assertLess(enqueue.index("std::unique_lock lock(m_lock);"), terminal_gate)
            for token in (
                "this->startWorkers();",
                "m_tasksTotal += 1;",
                "m_buckets[uint32_t(priority)].queue.emplace",
                "notifyWorkers(priority);",
            ):
                self.assertLess(terminal_gate, enqueue.index(token))

        self.assertLess(
            graphics.index("m_device->getDeviceStatus() == VK_ERROR_DEVICE_LOST"),
            graphics.index("pipeline->acquirePipeline();"),
        )

        worker_gate = worker.index(
            "m_device->getDeviceStatus() == VK_ERROR_DEVICE_LOST"
        )
        first_compile = worker.index("entry.pipelineLibrary->compilePipeline()")
        self.assertLess(worker.index("if (!m_workersRunning)"), worker_gate)
        self.assertLess(worker_gate, first_compile)
        self.assertLess(
            worker_gate,
            worker.index("entry.graphicsPipeline->compilePipeline(entry.graphicsState)"),
        )
        terminal_worker = worker[worker_gate:first_compile]
        self.assertNotIn("compilePipeline", terminal_worker)
        self.assertEqual(
            terminal_worker.count("entry.graphicsPipeline->releasePipeline();"), 1
        )
        self.assertEqual(terminal_worker.count("m_tasksCompleted += 1;"), 1)
        self.assertIn("continue;", terminal_worker)

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
            DIAGNOSTICS, "NotifyGpuDeviceLostFailStop("
        )
        self.assertIn("s_gpuDeviceLostIncidentLatched = true", notify)
        self.assertNotIn("s_gpuIncidentLatched = true", notify)
        self.assertIn("queue-error-device-lost-fail-stop", notify)
        self.assertIn(
            "queue-error-device-lost-device-fault-enrichment", notify
        )
        self.assertIn("s_gpuDeviceLostDeviceFaultEnrichmentLatched", notify)
        self.assertIn("deviceLossBaseTimestampMs", notify)
        self.assertIn("captureFinal", notify)
        self.assertLess(
            notify.index("if (!s_gpuDeviceLostIncidentLatched)"),
            notify.index("else if (!s_gpuDeviceLostDeviceFaultEnrichmentLatched"),
        )
        self.assertIn("s_gpuFlightBreadcrumb.load", notify)
        self.assertNotIn("RunWithActiveDevice", notify)
        self.assertIn('"firstErrorOrigin"', DIAGNOSTICS)

    def test_device_fault_extension_is_text_only_and_bounded(self):
        for source in (DXVK_DEVICE_INFO_H, DXVK_DEVICE_INFO_CPP):
            self.assertIn("extDeviceFault", source)
        self.assertIn(
            "ENABLE_EXT_FEATURE(extDeviceFault, deviceFault, false)",
            DXVK_DEVICE_INFO_CPP,
        )
        self.assertNotIn("deviceFaultVendorBinary", DXVK_DEVICE_INFO_CPP)
        self.assertIn("#ifdef VK_EXT_device_fault", VULKAN_LOADER)
        self.assertIn("VULKAN_FN(vkGetDeviceFaultInfoEXT)", VULKAN_LOADER)

        for token in (
            "MaxAddressInfos = 64u",
            "MaxVendorInfos  = 32u",
            "std::array<VkDeviceFaultAddressInfoEXT",
            "std::array<VkDeviceFaultVendorInfoEXT",
            "captureOnce(VkResult trigger)",
            "snapshot() const noexcept",
        ):
            self.assertIn(token, DXVK_DEVICE_FAULT_H)

        capture = function_body(
            DXVK_DEVICE_FAULT_CPP, "DxvkDeviceFaultCapture::captureOnce"
        )
        self.assertIn("trigger != VK_ERROR_DEVICE_LOST", capture)
        self.assertIn("counts.addressInfoCount = DxvkDeviceFaultSnapshot::MaxAddressInfos", capture)
        self.assertIn("counts.vendorInfoCount = DxvkDeviceFaultSnapshot::MaxVendorInfos", capture)
        self.assertIn("counts.vendorBinarySize = 0u", capture)
        self.assertIn("info.pVendorBinaryData = nullptr", capture)
        self.assertIn("result == VK_INCOMPLETE", capture)
        self.assertIn("const bool acceptedResult", capture)
        self.assertIn("if (acceptedResult)", capture)
        self.assertIn("m_addressInfoCount = 0u", capture)
        self.assertIn("m_vendorInfoCount = 0u", capture)
        self.assertIn("DxvkDeviceFaultCaptureState::Complete", capture)
        for forbidden in ("new ", "std::vector", "std::string", "mutex", "wait", "sleep", "Logger"):
            self.assertNotIn(forbidden, capture)

    def test_only_real_driver_results_request_device_fault_text(self):
        self.assertIn(
            "void notifyDeviceErrorFromDriverResult(VkResult status)",
            DXVK_DEVICE_H,
        )
        driver_entry = function_body(
            DXVK_DEVICE_H, "void notifyDeviceErrorFromDriverResult"
        )
        self.assertIn(
            "m_driverDeviceLossObserved.store(true, std::memory_order_release)",
            driver_entry,
        )
        self.assertIn("notifyDeviceError(status);", driver_entry)
        self.assertNotIn("m_deviceFault.captureOnce", driver_entry)
        self.assertNotIn("vkGetDeviceFaultInfoEXT", driver_entry)
        capture_entry = function_body(
            DXVK_DEVICE_H, "void captureDeviceFaultIfDriverLossObserved"
        )
        self.assertIn("m_driverDeviceLossObserved.load", capture_entry)
        self.assertIn("m_deviceFault.captureOnce(VK_ERROR_DEVICE_LOST)", capture_entry)
        self.assertIn("getDeviceFaultSnapshot() const noexcept", DXVK_DEVICE_H)

        submit = function_body(QUEUE, "DxvkSubmissionQueue::submitCmdLists")
        submit_result = submit.index("entry.submit.cmdList->submit")
        submit_fault = submit.index(
            "m_device->notifyDeviceErrorFromDriverResult(entry.result);"
        )
        submit_terminal = submit.index(
            "if (m_device->getDeviceStatus() == VK_ERROR_DEVICE_LOST)",
            submit_fault,
        )
        self.assertLess(submit_result, submit_fault)
        self.assertLess(submit_fault, submit_terminal)

        finish = function_body(QUEUE, "DxvkSubmissionQueue::finishCmdLists")
        wait = finish.index("vk->vkWaitSemaphores")
        wait_fault = finish.index(
            "m_device->notifyDeviceErrorFromDriverResult(status);"
        )
        self.assertLess(wait, wait_fault)
        self.assertIn("m_device->notifyDeviceError(VK_ERROR_DEVICE_LOST);", submit)
        self.assertIn("m_device->notifyDeviceError(status);", finish)
        self.assertNotIn("notifyDeviceErrorFromDriverResult", CS_THREAD)
        self.assertNotIn("m_driverDeviceLossObserved", CS_THREAD)
        self.assertNotIn("captureDeviceFaultIfDriverLossObserved", CS_THREAD)

        for source in (DXVK_DEVICE_CPP, MEMORY, PRESENTER):
            self.assertIn("notifyDeviceErrorFromDriverResult", source)

    def test_d3d_owner_captures_after_base_incident_and_enriches_once(self):
        self.assertIn(
            "std::atomic<bool> m_vkDeviceLostBaseIncidentReady{false}",
            DEVICE_H,
        )
        latch = function_body(
            DEVICE_CPP, "D3D9DeviceEx::CheckVulkanDeviceLostFailStop"
        )
        base_notify = latch.index(
            "origin, deviceFaultBeforeCapture"
        )
        capture = latch.index("captureDeviceFaultIfDriverLossObserved")
        enrichment_notify = latch.index(
            "origin, deviceFaultAfterCapture"
        )
        base_ready = latch.index(
            "m_vkDeviceLostBaseIncidentReady.store(true, std::memory_order_release)"
        )
        self.assertLess(base_notify, capture)
        self.assertLess(base_notify, base_ready)
        self.assertLess(base_ready, capture)
        self.assertLess(capture, enrichment_notify)
        self.assertIn("if (firstFailStop)", latch)
        self.assertIn(
            "else if (!m_vkDeviceLostBaseIncidentReady.load(\n"
            "                 std::memory_order_acquire))",
            latch,
        )
        self.assertLess(
            latch.index("else if (!m_vkDeviceLostBaseIncidentReady.load"),
            capture,
        )
        self.assertLess(
            latch.index("m_war3ShadowSessionReady.store(false"), base_notify
        )

    def test_terminal_flight_poll_cannot_consume_device_lost_incident(self):
        flight = function_body(DIAGNOSTICS, "void RecordGpuFlightFrame")
        self.assertIn("const bool terminalQueueFailure", flight)
        self.assertIn("!terminalQueueFailure", flight)
        self.assertNotIn("s_gpuDeviceLostIncidentLatched", flight)
        self.assertNotIn("NotifyGpuDeviceLostFailStop", flight)
        self.assertNotIn("queue-error-device-lost-fail-stop", flight)

    def test_device_fault_json_owns_bounded_text_without_vendor_binary(self):
        self.assertNotIn(
            "VK_EXT_device_fault is not exposed by this DXVK build",
            DIAGNOSTICS,
        )
        for token in (
            '"supported"',
            '"captureState"',
            '"complete"',
            '"queryResult"',
            '"truncated"',
            '"addressInfos"',
            '"vendorInfos"',
            '"vendorBinaryEnabled", false',
            "BoundedDeviceFaultText",
            "MaxAddressInfos",
            "MaxVendorInfos",
            "hasDeviceFaultData",
        ):
            self.assertIn(token, DIAGNOSTICS)
        json_start = DIAGNOSTICS.index("const bool hasDeviceFaultData")
        address_loop = DIAGNOSTICS.index("for (uint32_t index = 0u; index < addressInfoCount")
        self.assertLess(json_start, address_loop)
        self.assertIn("hasDeviceFaultData ?", DIAGNOSTICS[json_start:address_loop])
        self.assertNotIn("NV checkpoint", DIAGNOSTICS)


if __name__ == "__main__":
    unittest.main()
