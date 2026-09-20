from pathlib import Path
import unittest
ROOT=Path(__file__).resolve().parents[1]
def read(p):return (ROOT/p).read_text(encoding='utf-8')
class ShortcutContracts(unittest.TestCase):
    def test_live_callback_before_ime(self):
        w=read('src/d3d9/d3d9_window.cpp').split('LRESULT CALLBACK D3D9WindowProc(',1)[1]
        self.assertLess(w.index('HandleFrameHistoryShortcut(message'),w.index('IsImeMessage(message)'))
        self.assertIn('TestFrameHistoryWindowShortcut()',w)
        self.assertIn('SendMessageW(target,kHistoryShortcutTestMessage',w)
        self.assertIn('message==kHistoryShortcutTestMessage&&g_historyShortcutTest',w)
    def test_old_uninstalled_callback_not_authority(self):
        s=read('src/d3d9/d3d9_swapchain.cpp')
        self.assertNotIn('TriggerFrameHistoryFromGame()',s)
        self.assertNotIn('wParam==VK_F8',s)
    def test_control_shift_c_truth_selector(self):
        s=read('src/d3d9/war3/tools/war3_frame_history_core.h')
        self.assertIn('key==0x43 && ctrl && shift && !repeat',s)
    def test_recorder_display_follows_debug_window(self):
        s=read('src/d3d9/war3/ui/war3_imgui.cpp')
        render=s.split('void War3Imgui::render(bool inScene)',1)[1].split('void War3Imgui::drawFrameRecorderPanel()',1)[0]
        panel=s.split('void War3Imgui::drawFrameRecorderPanel()',1)[1].split('void War3Imgui::drawDebugWindow()',1)[0]
        console=s.split('void War3Imgui::drawDebugWindow()',1)[1]
        self.assertIn('!m_initialized || m_hasRendered || !m_visible',render)
        self.assertNotIn('WarVK recorder status',render)
        self.assertIn('drawFrameRecorderPanel();',console)
        self.assertIn('Ctrl+Shift+C',panel)
        self.assertIn('ImGui::CollapsingHeader(',panel)
        self.assertIn('recorder.bufferedMs / 1000.0f',panel)
    def test_hud_does_not_enter_history_pixels(self):
        s=read('src/d3d9/d3d9_swapchain.cpp')
        self.assertIn('if(m_frameHistory&&!afterUi)',s)
        self.assertIn('CaptureNativeAsyncScreenshot(true)',s)
        self.assertIn('prepare(image,!afterUi)',s)
        self.assertIn('if (advanceBurst&&m->burstRemaining.load())',read('src/d3d9/war3/tools/war3_async_screenshot.cpp'))
    def test_one_second_is_not_a_fixed_frame_claim(self):
        s=read('src/d3d9/war3/tools/war3_frame_history.cpp')
        self.assertIn('newest-oldest>=uint64_t(preMillis)*frequency/1000',s)
        self.assertIn('getMemoryHeapInfo()',s)
        self.assertIn('insufficient-video-memory-headroom',s)
        self.assertIn('"durationSatisfied"',s)
    def test_watcher_pid_reuse_and_normal_disconnect(self):
        s=read('AutoTest/frame_history_watch.py')
        self.assertIn("peek.get('processNonce')!=cpu['processNonce']",s)
        self.assertIn("peek.get('session')!=session",s)
        self.assertIn('time.monotonic()-last_reply>5',s)
        self.assertNotIn('Ctrl+Shift+F8',s)
if __name__=='__main__':unittest.main()
