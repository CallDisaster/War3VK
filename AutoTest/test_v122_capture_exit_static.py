"""Candidate source guards, not player exit or point-light acceptance."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


def source(path):
    return (ROOT/path).read_text(encoding='utf-8')


class CaptureExitContracts(unittest.TestCase):
    def test_recording_independent_defaults(self):
        text = source('src/d3d9/war3/hooks/war3_native_capture.cpp')
        self.assertIn('(!persistent || !std::strcmp(persistent,"1"))', text)
        self.assertIn('(!elide || !std::strcmp(elide,"1"))', text)
        self.assertNotIn('isRecording()', text)
        self.assertIn('g_nativeFrameSyncTrampoline(object, elided ? 0 : request)', text)
        self.assertIn('GetMainLoopThreadId()', text)

    def test_console_is_explicit_opt_in(self):
        text = source('src/d3d9/d3d9_war3_debug.h')
        self.assertIn('DXVK_WAR3_DEBUG_CONSOLE', text)
        self.assertIn('GetConsoleWindow()', text)
        self.assertIn('AllocConsole()', text)
        self.assertIn('OutputDebugStringA', text)
        self.assertIn('ConsoleOutputEnabled()', text)

    def test_control_thread_process_detach_only(self):
        text = source('src/d3d9/war3/tools/war3_control_plane.cpp')
        deleter = text.split('struct ControlThreadDeleter {', 1)[1].split('\n};', 1)[0]
        guard = deleter.split('if (this_thread::isInModuleDetachment()) {', 1)[1].split('}', 1)[0]
        self.assertIn('return;', guard)
        self.assertNotIn('join(', guard)
        self.assertNotIn('detach(', guard)
        self.assertNotIn('delete ', guard)
        self.assertIn('delete thread;', deleter)
        self.assertIn('g_serverThread->join();', text)
        self.assertIn('g_stopRequested.store(true', text)
        self.assertIn('HANDLE wake = CreateFileA', text)
        self.assertNotIn('TerminateProcess(', text)

    def test_exit_native_strict_and_private(self):
        text = source('src/d3d9/war3/hooks/war3_jass_command_bridge.cpp')
        helper = text.split('bool EndJassGameForExitTest(', 1)[1].split('\nJassCameraFieldTestResult', 1)[0]
        self.assertIn('ResolveNativeStrict("EndGame", "(B)V", 1u', helper)
        self.assertIn('reinterpret_cast<NativeVoidBooleanFn>(function)(0u)', helper)
        self.assertNotIn('ExitProcess(', helper)
        api = source('src/d3d9/war3/tools/war3_internal_test_api.cpp')
        gate = api.split('command == "game.end_for_exit_test"', 1)[1].split('command == "shutdown.session"', 1)[0]
        self.assertIn('env::getEnvVar("DXVK_WAR3_INTERNAL_EXIT_TEST") != "1"', gate)
        self.assertIn('response["processExitProven"] = false', gate)

    def test_perf_owner_not_destructed_after_tls_at_process_exit(self):
        text = source('src/d3d9/war3/tools/war3_perf_monitor.cpp')
        owner = text.split('War3PerfMonitor &War3PerfMonitor::instance()', 1)[1].split('War3PerfMonitor::War3PerfMonitor()', 1)[0]
        self.assertIn('std::unique_ptr<War3PerfMonitor, decltype(destroy)>', owner)
        self.assertLess(owner.index('isInModuleDetachment()'), owner.index('delete monitor;'))
        device = source('src/d3d9/d3d9_device.cpp').split('D3D9DeviceEx::~D3D9DeviceEx()', 1)[1].split('void D3D9DeviceEx::War3AttachGpuSkinNativeBridge', 1)[0]
        self.assertLess(device.index('isInModuleDetachment()'), device.index('War3PerfMonitor::instance()'))
        self.assertIn('perfMonitor.shutdown();', device)
        self.assertIn('SynchronizeCsThread(DxvkCsThread::SynchronizeAll);', device)
        self.assertIn('m_dxvkDevice->waitForIdle();', device)

    def test_runner_zero_exit_no_global_input(self):
        text = source('AutoTest/run_v122_native_capture_gate.py')
        self.assertIn("natural_proof.get('exitCode') != 0", text)
        self.assertIn('baseline_width=2560,baseline_height=1440', text)
        self.assertIn('use_isolated_desktop=True', text)
        self.assertIn('force=False', text)
        self.assertIn("receipt['playerUnchanged']", text)
        self.assertIn('no overwrite/retry', text)


if __name__ == '__main__':
    unittest.main()
