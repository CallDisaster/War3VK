import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class Contract(unittest.TestCase):
    def test_observer_only(self):
        s = (ROOT / 'src/d3d9/war3/tools/war3_frame_timeline.cpp').read_text(encoding='utf-8')
        for x in [
            'DXVK_WAR3_FRAME_TIMELINE',
            'coverageComplete\\":false',
            'legacySelf',
            'otherPresents',
            'GetThreadTimes',
            'QueryPerformanceCounter',
        ]:
            self.assertIn(x, s)
        for x in ['WriteFile(', 'fopen(', 'SuspendThread(', 'Sleep(', 'TerminateProcess(']:
            self.assertNotIn(x, s)

    def test_native_scopes(self):
        s = (ROOT / 'src/d3d9/war3/hooks/war3_hook_lifecycle.cpp').read_text(encoding='utf-8')
        self.assertEqual(s.count('::dxvk::war3::timeline::Scope wallTimeline('), 17)
        self.assertNotIn('::dxvk::war3::timeline::Enabled() ||', s)
        self.assertIn('kNativeMainLoopDeepPhaseHookEnabled', s)
        self.assertIn('War3PerfHookLevel() >= 2', s)

    def test_present_boundary(self):
        s = (ROOT / 'src/d3d9/d3d9_swapchain.cpp').read_text(encoding='utf-8')
        begin = s.index('BeginNativeFrameSyncPresent(this)')
        present = s.index('timeline::Present(this')
        entry = s.index('entry-before-lock')
        self.assertLess(begin, present)
        self.assertLess(present, entry)
        self.assertEqual(s.count('timeline::PresentResult('), 10)

    def test_raii_and_json(self):
        s = (ROOT / 'src/d3d9/war3/tools/war3_perf_monitor.cpp').read_text(encoding='utf-8')
        self.assertEqual(s.count('timeline::Leave(m_timelineToken);'), 2)
        self.assertEqual(s.count('other.m_timelineToken = 0;'), 2)
        self.assertIn('json << "  \\"mainThreadTimeline\\": "', s)
        self.assertIn('json << "  \\"dataCollectionTree\\": "', s)
        self.assertIn('"DXVK_WAR3_FRAME_TIMELINE"', s)

    def test_resource_wait_observer_keeps_operation_order(self):
        s = (ROOT / 'src/d3d9/d3d9_device.cpp').read_text(encoding='utf-8')
        f = s[s.index('bool D3D9DeviceEx::WaitForResource('):
              s.index('bool D3D9DeviceEx::War3ShouldDrawDebugOverlay()')]
        for marker in [
            'D3D9/WaitForResource',
            'D3D9/ResourceCsSync',
            'D3D9/ResourceWaitFlush',
            'D3D9/ResourceGpuWait',
        ]:
            self.assertIn(marker, f)
        self.assertEqual(f.count('m_dxvkDevice->waitForResource(Resource, access);'), 1)
        self.assertEqual(f.count('SynchronizeCsThread(SequenceNumber);'), 2)
        self.assertLess(f.index('Flush();'), f.rindex('SynchronizeCsThread(SequenceNumber);'))
        self.assertLess(
            f.rindex('SynchronizeCsThread(SequenceNumber);'),
            f.index('m_dxvkDevice->waitForResource'))
        self.assertIn('if (MapFlags & D3DLOCK_DONOTWAIT)', f)
        self.assertIn('return false;', f)
        image = s[s.index('HRESULT D3D9DeviceEx::LockImage('):
                  s.index('HRESULT D3D9DeviceEx::LockImage(') + 18000]
        self.assertIn('war3::timeline::Scope imageTimeline("D3D9/LockImage");', image)
        self.assertIn(
            'if (unlikely(needsReadback)) {\n    war3::timeline::Scope readbackTimeline',
            image)


if __name__ == '__main__':
    unittest.main()
