#!/usr/bin/env python3
"""Static contracts for direct Vulkan device-lost provenance at P5a sites."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
CMDLIST = (ROOT / "src/dxvk/dxvk_cmdlist.cpp").read_text(encoding="utf-8")
COMPUTE = (ROOT / "src/dxvk/dxvk_compute.cpp").read_text(encoding="utf-8")
GRAPHICS = (ROOT / "src/dxvk/dxvk_graphics.cpp").read_text(encoding="utf-8")
SHADER = (ROOT / "src/dxvk/dxvk_shader.cpp").read_text(encoding="utf-8")
GPU_QUERY = (ROOT / "src/dxvk/dxvk_gpu_query.cpp").read_text(encoding="utf-8")
GPU_EVENT = (ROOT / "src/dxvk/dxvk_gpu_event.cpp").read_text(encoding="utf-8")


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


def assert_before(test: unittest.TestCase, body: str, first: str, second: str,
                  start: int = 0) -> int:
    first_index = body.index(first, start)
    second_index = body.index(second, first_index)
    test.assertLess(first_index, second_index)
    return second_index


class VulkanDeviceLostSourceProvenanceStaticTests(unittest.TestCase):
    def test_command_pool_preserves_all_direct_results_before_throwing(self):
        primary = function_body(CMDLIST, "DxvkCommandPool::getCommandBuffer")
        allocation = primary.index("VkResult vr = vk->vkAllocateCommandBuffers")
        allocation_notify = assert_before(
            self, primary, "m_device->notifyDeviceErrorFromDriverResult(vr);",
            "throw DxvkError(\"DxvkCommandPool: Failed to allocate command buffer\")",
            allocation,
        )
        begin = primary.index("VkResult vr = vk->vkBeginCommandBuffer")
        self.assertLess(allocation_notify, begin)
        assert_before(
            self, primary, "m_device->notifyDeviceErrorFromDriverResult(vr);",
            "throw DxvkError(\"DxvkCommandPool: Failed to begin command buffer\")",
            begin,
        )

        secondary = function_body(
            CMDLIST, "DxvkCommandPool::getSecondaryCommandBuffer"
        )
        allocation = secondary.index("VkResult vr = vk->vkAllocateCommandBuffers")
        allocation_notify = assert_before(
            self, secondary, "m_device->notifyDeviceErrorFromDriverResult(vr);",
            "throw DxvkError(\"DxvkCommandPool: Failed to allocate secondary command buffer\")",
            allocation,
        )
        begin = secondary.index("VkResult vr = vk->vkBeginCommandBuffer")
        self.assertLess(allocation_notify, begin)
        assert_before(
            self, secondary, "m_device->notifyDeviceErrorFromDriverResult(vr);",
            "throw DxvkError(\"DxvkCommandPool: Failed to begin secondary command buffer\")",
            begin,
        )

        reset = function_body(CMDLIST, "DxvkCommandPool::reset()")
        self.assertIn("VkResult vr = vk->vkResetCommandPool", reset)
        notify = assert_before(
            self, reset, "m_device->notifyDeviceErrorFromDriverResult(vr);",
            "throw DxvkError(\"DxvkCommandPool: Failed to reset command pool\")",
        )
        self.assertLess(notify, reset.index("m_nextPrimary = 0"))

    def test_command_list_end_results_are_reported_before_state_progression(self):
        secondary = function_body(
            CMDLIST, "DxvkCommandList::endSecondaryCommandBuffer"
        )
        self.assertIn("VkResult vr = m_vkd->vkEndCommandBuffer", secondary)
        notify = assert_before(
            self, secondary, "m_device->notifyDeviceErrorFromDriverResult(vr);",
            "throw DxvkError(\"DxvkCommandList: Failed to end secondary command buffer\")",
        )
        self.assertLess(
            notify,
            secondary.index("m_cmd.cmdBuffers[uint32_t(DxvkCmdBuffer::ExecBuffer)]"),
        )

        primary = function_body(CMDLIST, "DxvkCommandList::endCommandBuffer")
        self.assertIn("VkResult vr = vk->vkEndCommandBuffer", primary)
        assert_before(
            self, primary, "m_device->notifyDeviceErrorFromDriverResult(vr);",
            "throw DxvkError(\"DxvkCommandList: Failed to end command buffer\")",
        )

    def test_compute_pipeline_reports_the_driver_result_before_logging(self):
        body = function_body(COMPUTE, "DxvkComputePipeline::createPipeline")
        self.assertIn("VkResult vr = vk->vkCreateComputePipelines", body)
        assert_before(
            self, body, "m_device->notifyDeviceErrorFromDriverResult(vr);",
            "Logger::err(",
        )

    def test_graphics_pipeline_failures_preserve_compile_required_behavior(self):
        for signature, error in (
            (
                "DxvkGraphicsPipelineVertexInputLibrary::DxvkGraphicsPipelineVertexInputLibrary",
                "throw DxvkError(\"Failed to create vertex input pipeline library\")",
            ),
            (
                "DxvkGraphicsPipelineFragmentOutputLibrary::DxvkGraphicsPipelineFragmentOutputLibrary",
                "throw DxvkError(\"Failed to create vertex input pipeline library\")",
            ),
        ):
            body = function_body(GRAPHICS, signature)
            self.assertIn("VkResult vr = vk->vkCreateGraphicsPipelines", body)
            assert_before(
                self, body, "m_device->notifyDeviceErrorFromDriverResult(vr);",
                error,
            )

        base = function_body(GRAPHICS, "DxvkGraphicsPipeline::createBasePipeline")
        self.assertIn("VkResult vr = vk->vkCreateGraphicsPipelines", base)
        self.assertIn(
            "vr != VK_SUCCESS && vr != VK_PIPELINE_COMPILE_REQUIRED_EXT",
            base,
        )
        assert_before(
            self, base, "m_device->notifyDeviceErrorFromDriverResult(vr);",
            "Logger::err(",
        )

        optimized = function_body(
            GRAPHICS, "DxvkGraphicsPipeline::createOptimizedPipeline"
        )
        self.assertIn("VkResult vr = vk->vkCreateGraphicsPipelines", optimized)
        assert_before(
            self, optimized, "m_device->notifyDeviceErrorFromDriverResult(vr);",
            "Logger::err(",
        )

    def test_shader_pipeline_libraries_report_only_direct_failure_results(self):
        for signature, create, error in (
            (
                "DxvkShaderPipelineLibrary::compileVertexShaderPipeline",
                "VkResult vr = vk->vkCreateGraphicsPipelines",
                "Logger::err(",
            ),
            (
                "DxvkShaderPipelineLibrary::compileFragmentShaderPipeline",
                "VkResult vr = vk->vkCreateGraphicsPipelines",
                "Logger::err(",
            ),
            (
                "DxvkShaderPipelineLibrary::compileComputeShaderPipeline",
                "VkResult vr = vk->vkCreateComputePipelines",
                "Logger::err(",
            ),
        ):
            body = function_body(SHADER, signature)
            self.assertIn(create, body)
            self.assertIn(
                "vr != VK_SUCCESS && vr != VK_PIPELINE_COMPILE_REQUIRED_EXT",
                body,
            )
            assert_before(
                self, body, "m_device->notifyDeviceErrorFromDriverResult(vr);", error
            )

    def test_query_and_event_paths_report_direct_failure_before_existing_failure(self):
        query = function_body(
            GPU_QUERY, "DxvkQuery::accumulateQueryDataForGpuQueryLocked"
        )
        self.assertIn("VkResult result = vk->vkGetQueryPoolResults", query)
        self.assertLess(query.index("result == VK_NOT_READY"), query.index("result != VK_SUCCESS"))
        assert_before(
            self, query, "m_device->notifyDeviceErrorFromDriverResult(result);",
            "return DxvkGpuQueryStatus::Failed;",
        )

        allocator = function_body(
            GPU_QUERY, "DxvkGpuQueryAllocator::createQueryPool"
        )
        self.assertIn("VkResult vr = vk->vkCreateQueryPool", allocator)
        assert_before(
            self, allocator, "m_device->notifyDeviceErrorFromDriverResult(vr);",
            "Logger::err(",
        )

        event = function_body(GPU_EVENT, "DxvkEvent::test()")
        status = event.index("m_status = vk->vkGetEventStatus")
        notify = event.index(
            "m_device->notifyDeviceErrorFromDriverResult(m_status);", status
        )
        self.assertLess(status, notify)
        self.assertLess(notify, event.index("switch (m_status)"))

    def test_p5a_sources_never_capture_or_write_incidents(self):
        for source in (CMDLIST, COMPUTE, GRAPHICS, SHADER, GPU_QUERY, GPU_EVENT):
            self.assertNotIn("captureDeviceFaultIfDriverLossObserved", source)
            self.assertNotIn("vkGetDeviceFaultInfoEXT", source)
            self.assertNotIn("NotifyGpuDeviceLostFailStop", source)


if __name__ == "__main__":
    unittest.main()
