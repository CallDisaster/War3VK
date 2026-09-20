from pathlib import Path
import unittest
ROOT=Path(__file__).resolve().parents[1]
H=(ROOT/'src/d3d9/war3/tools/war3_frame_history.cpp').read_text(encoding='utf-8')
S=(ROOT/'src/d3d9/war3/tools/war3_async_screenshot.cpp').read_text(encoding='utf-8')
W=(ROOT/'src/d3d9/d3d9_swapchain.cpp').read_text(encoding='utf-8')
WINDOW=(ROOT/'src/d3d9/d3d9_window.cpp').read_text(encoding='utf-8')
class Contracts(unittest.TestCase):
    def test_no_capture_time_io(self):
        capture=H.split('FrameHistory::Capture FrameHistory::capture',1)[1].split('FrameHistory::Export',1)[0]
        for forbidden in ('CreateFile','WriteFile','saveNew','std::to_wstring','make_shared','mapPtr'):
            # Requests are preallocated during PendingArm only.
            steady=capture.split('if(m->width!=info.extent.width',1)[1]
            self.assertNotIn(forbidden,steady)
    def test_readback_only_after_trigger(self):
        self.assertIn('if(m->state==Impl::Frozen){m->state=Impl::Exporting',H)
        self.assertIn('if(m->state!=Impl::Exporting)return {}',H)
    def test_complete_queries_before_read(self):
        self.assertIn('screenshot::Readable(query == VK_SUCCESS, deviceOk, completed, slot.value)',S)
        self.assertIn('slot.history->observedValue=completed',S)
        self.assertIn('job->targetValue=++slot.value',S)
    def test_budget_and_epochs(self):
        self.assertIn('history::Budget(',H);self.assertIn('m->epoch!=key.mapEpoch',H)
        self.assertIn('m->deviceEpoch!=key.deviceEpoch',H)
    def test_no_gpu_wait(self):
        for token in ('vkDeviceWaitIdle','vkQueueWaitIdle','LockRect','vkWaitSemaphores'):
            self.assertNotIn(token,H)
    def test_hotkey_is_game_window_only(self):
        self.assertIn('tools::HandleFrameHistoryShortcut(message',WINDOW)
        self.assertIn('GetKeyState(VK_SHIFT)',WINDOW)
        self.assertNotIn('TriggerFrameHistoryFromGame()',W)
        self.assertIn('TestFrameHistoryWindowShortcut',WINDOW)
        self.assertNotIn('RegisterHotKey',H);self.assertNotIn('GetAsyncKeyState',H)
    def test_create_new_and_preserve_player(self):
        self.assertIn('CREATE_NEW',H);self.assertNotIn('REPLACE_EXISTING',H)
    def test_owner_release_and_defaults(self):
        self.assertIn('if(!evidence::Enabled())return {}',H)
        self.assertIn('if(p&&r.owner==p)',H)
    def test_cancel_and_quarantine(self):
        # 2026-09-19：取消现在**带具名原因**（交换链重置/拷贝失败/源缺失/异常），
        # 意图不变：这些路径仍必须取消历史录制；并加强为要求四处具名原因齐全。
        self.assertIn('m_frameHistory->cancel("',W)
        for _reason in ('swapchain-reset','history-copy-submit-failed','history-source-image-missing','history-capture-exception'):
            self.assertIn(_reason,W)
        self.assertIn('m_asyncScreenshot->quarantine(copy->slot)',W)
    def test_metadata_does_not_claim_full_capture(self):
        self.assertIn('{"captureComplete",false}',H);self.assertIn('{"rootCauseReady",false}',H)
if __name__=='__main__':unittest.main()
