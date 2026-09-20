"""Wiring guard only. Runtime CPU probes are mandatory Meson targets alongside it."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
def read(path):
    return (ROOT / path).read_text(encoding="utf-8")

class Maintenance(unittest.TestCase):
    def test_image_unpublished_handle_has_single_owner(self):
        source = read("src/dxvk/dxvk_memory.cpp")
        body = source.split("DxvkMemoryAllocator::createImageResource(", 1)[1].split("return allocation;", 1)[0]
        self.assertEqual(body.count("vkDestroyImage("), 1)
        self.assertLess(body.index("DxvkUnboundResourceGuard unboundImage"), body.index("vkGetImageMemoryRequirements2"))
        self.assertLess(body.index("allocation->m_image = image"), body.index("unboundImage.release()"))
        self.assertLess(body.index("unboundImage.release()"), body.index("vkBindImageMemory"))
        self.assertNotIn("catch (", body)

    def test_both_image_constructors_publish_after_storage(self):
        source = read("src/dxvk/dxvk_image.cpp")
        body = source.split("DxvkImage::DxvkImage(", 1)[1].split("DxvkImage::~DxvkImage()", 1)[0]
        self.assertEqual(body.count("DxvkPublishInitialImageStorage("), 2)
        self.assertEqual(body.count("m_allocator->registerResource(this)"), 2)
        self.assertEqual(body.count("[this] { m_allocator->registerResource(this); }"), 2)
        self.assertNotIn("vkDestroyImage", body)  # imported image is not locally owned

    def test_requests_not_idle_prepare_allocate_screenshot_backing(self):
        source = read("src/d3d9/war3/tools/war3_async_screenshot.cpp")
        prepare = source.split("void AsyncScreenshot::prepare(", 1)[1].split("std::optional<AsyncScreenshot::Copy> AsyncScreenshot::take(", 1)[0]
        self.assertNotIn("createBuffer(", prepare)
        self.assertNotIn("createFence(", prepare)
        self.assertNotIn("prepareRequestedSlot(", prepare)
        reclaim = source.split("void releaseRetiredSlots()", 1)[1].split("bool prepareRequestedSlot(", 1)[0]
        self.assertLess(reclaim.index("State::Retired, State::Preparing"), reclaim.index("slot.buffer = nullptr"))
        self.assertLess(reclaim.index("slot.fence = nullptr"), reclaim.index("State::Free"))
        self.assertNotIn("State::Submitted", reclaim)
        self.assertIn("m->releaseRetiredSlots();", prepare)
        begin = source.split("void AsyncScreenshot::beginPresent()", 1)[1].split("uint64_t AsyncScreenshot::presentOrdinal()", 1)[0]
        self.assertIn("m->releaseRetiredSlots();", begin)
        swap = read("src/d3d9/d3d9_swapchain.cpp").split("D3D9SwapChainEx::Present(", 1)[1].split("D3D9SwapChainEx::PresentImageGDI", 1)[0]
        self.assertLess(swap.index("LockDevice()"), swap.index("m_asyncScreenshot->beginPresent()"))
        take = source.split("AsyncScreenshot::take(uint32_t index)", 1)[1].split("AsyncScreenshot::takeHistory(", 1)[0]
        self.assertLess(take.index("State::Requested, State::Preparing"), take.index("prepareRequestedSlot(slot)"))
        self.assertLess(take.index("prepareRequestedSlot(slot)"), take.index("return Copy{"))
        history = source.split("AsyncScreenshot::takeHistory(", 1)[1].split("void AsyncScreenshot::submitted", 1)[0]
        self.assertLess(history.index("State::Free,State::Preparing"), history.index("prepareRequestedSlot(slot)"))
        self.assertLess(history.index("prepareRequestedSlot(slot)"), history.index("job->queued.store(true"))

    def test_standard_suite_includes_actual_production_and_original_offline_regression(self):
        meson = read("src/d3d9/meson.build")
        for name in ("image_allocation_unwind", "screenshot_cold_production", "native_async_screenshot_core", "d3d9_memory_allocator_production", "d3d9_memory_chunk_tail_offline"):
            self.assertIn("test('" + name + "'", meson)
        self.assertIn("'../../AutoTest/test_d3d9_memory_allocator_production.cpp', 'd3d9_mem.cpp'", meson)

if __name__ == "__main__":
    unittest.main()
