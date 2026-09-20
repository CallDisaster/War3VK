"""Production wiring guard, not runtime/GPU recovery acceptance."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]

class Wiring(unittest.TestCase):
    def test_all_three_streams_use_same_retention_contract(self):
        s = (ROOT / 'src/d3d9/d3d9_device.cpp').read_text(encoding='utf-8')
        for stream in ('posBytes', 'uvBytes', 'idxBytes'):
            self.assertIn(f'{stream}, snapshotLifetime, snapshotPage', s)
        self.assertEqual(s.count('const auto snapshotLifetime ='), 1)
        allocator = s.split('D3D9DeviceEx::War3AllocateStage11Snapshot(', 1)[1].split(
            'void D3D9DeviceEx::War3CollectUnusedStage11SnapshotPages()', 1)[0]
        for text in ('request.sameRetentionIntentOnly = true', 'page->mapEpoch',
                     'page->deviceEpoch', 'plan.mixedLifetimeBorrow',
                     'request.pageCreateGateOpen = false', 'plan = choosePage()',
                     'snapshot-lifetime/v1'):
            self.assertIn(text, allocator)
        self.assertEqual(allocator.count('m_dxvkDevice->createBuffer('), 1)
        self.assertNotIn('catch (...)', allocator)
        self.assertNotIn('catch (const DxvkError', allocator)
        self.assertEqual(allocator.count('getDeviceStatus() != VK_SUCCESS'), 1)
        # Internal suballocation/object-pool bad_alloc is not transactionally
        # recoverable yet; never swallow the whole createBuffer call.
        self.assertNotIn('catch (const std::bad_alloc', allocator)
        self.assertNotIn('page->used = 0', allocator)
        self.assertNotIn('waitForIdle', allocator)
        self.assertNotIn('vkFreeMemory', allocator)

    def test_birth_before_publication_and_unwind_before_registration(self):
        s = (ROOT / 'src/dxvk/dxvk_buffer.cpp').read_text(encoding='utf-8')
        constructors = s.split('DxvkBuffer::~DxvkBuffer()', 1)[0]
        self.assertEqual(constructors.count('DxvkPublishInitialBufferStorage('), 2)
        self.assertEqual(constructors.count('m_allocator->registerResource(this);'), 2)
        self.assertEqual(constructors.count('[this] { m_allocator->registerResource(this); }'), 2)
        guard = (ROOT / 'src/dxvk/dxvk_buffer_allocation_guard.h').read_text()
        self.assertLess(guard.index('if (!storage)'), guard.index('assign(std::move(storage))'))
        self.assertLess(guard.index('assign(std::move(storage))'), guard.index('publish();'))
        memory = (ROOT / 'src/dxvk/dxvk_memory.cpp').read_text()
        creation = memory.split('DxvkMemoryAllocator::createBufferResource(', 1)[1].split(
            'DxvkMemoryAllocator::createImageResource(', 1)[0]
        self.assertLess(creation.index('DxvkUnboundBufferGuard unboundBuffer'),
                        creation.index('vkGetBufferMemoryRequirements2'))
        self.assertLess(creation.index('allocation->m_buffer = buffer'),
                        creation.index('unboundBuffer.release()'))
        self.assertLess(creation.index('unboundBuffer.release()'), creation.index('vkBindBufferMemory'))
        self.assertEqual(creation.count('throw DxvkBufferAllocationError'), 2)
        self.assertEqual(creation.count('vr == VK_ERROR_OUT_OF_HOST_MEMORY || vr == VK_ERROR_OUT_OF_DEVICE_MEMORY'), 2)
        low = memory.split('DxvkMemoryAllocator::allocateDeviceMemory(', 1)[1].split(
            'DxvkMemoryAllocator::createAllocation(', 1)[0]
        self.assertIn('const VkResult status = vk->vkAllocateMemory(', low)
        self.assertIn('notifyDeviceErrorFromDriverResult(status)', low)
        self.assertIn('status != VK_ERROR_OUT_OF_HOST_MEMORY &&', low)
        self.assertIn('throw DxvkError(str::format("Failed to allocate device memory: ", status))', low)

if __name__ == '__main__':
    unittest.main()
