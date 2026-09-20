from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
def read(path): return (ROOT/path).read_text(encoding='utf-8')

class Wiring(unittest.TestCase):
    def test_worker_uses_resolved_profile_and_joins_outside_registry_lock(self):
        s=read('src/d3d9/war3/tools/war3_frame_history.cpp')
        self.assertIn('return evidence::LocalRecorderEnabled()',s)
        self.assertIn('if(!evidence::Enabled())return {}',s)
        release=s.split('void FrameHistory::Release',1)[1].split('void FrameHistory::cancel',1)[0]
        self.assertLess(release.index('p->stopRecorder();'),release.index('r.owner.reset();'))
        self.assertIn('if(r.owner!=p)return;p->cancel();}',release)
        self.assertIn('m->worker.join()',s)
        self.assertNotIn('.detach()',s)
    def test_worker_waits_for_images_and_frozen_writers(self):
        s=read('src/d3d9/war3/tools/war3_frame_history.cpp')
        self.assertIn('evidence::FrozenForExport(session.session())',s)
        self.assertIn('session.resetEndsSession(m->cancelled.load())',s)
        s=s.split('void FrameHistory::runRecorder()',1)[1].split('FrameHistory::Capture',1)[0]
        self.assertIn('IsInGameRenderReady()',s)
        self.assertIn('"export_manifest"',s);self.assertIn(r'L"\\incident.json"',s)
        self.assertIn('cpu["inputs"].value("ok",false)',s)
        for token in ('copyImage','vkWait','LockRect','frames['):self.assertNotIn(token,s)
    def test_hotkey_is_queue_not_trylock(self):
        s=read('src/d3d9/war3/tools/war3_frame_history.cpp')
        hot=s.split('bool TriggerFrameHistoryFromGame()',1)[1].split('bool HandleFrameHistoryShortcut',1)[0]
        self.assertIn('shortcut.request()',hot);self.assertNotIn('mutex',hot)
        self.assertIn('shortcut.consume(m->session)',s)
    def test_control_ownership_and_cancellable_streaming(self):
        s=read('src/d3d9/war3/tools/war3_frame_evidence.cpp')
        self.assertIn('if(!controlAuthority.allows(localLease,action=="status"))',s)
        self.assertIn('return controlAuthority.claim(lease,!s||s->ring.state()==State::Idle)',s)
        self.assertIn('return controlAuthority.release(lease)',s)
        self.assertNotIn('localRecorderOwned',s)
        self.assertIn('ring.visitFrozen',s);self.assertNotIn('snapshot(action=="export")',s)
        self.assertIn('stopping->load(std::memory_order_relaxed)',s)
        self.assertIn('"skinPaletteContract",war3::render::skin::ContractEnabled()',s)
    def test_precise_lease_binding_and_join_before_release(self):
        header=read('src/d3d9/war3/tools/war3_frame_evidence_control.h')
        self.assertNotIn('bool localRecorder',header)
        self.assertIn('const RecorderControlLease* localLease=nullptr',header)
        owner=read('src/d3d9/war3/tools/war3_frame_history.cpp')
        stop=owner.split('void FrameHistory::stopRecorder()',1)[1].split('void FrameHistory::runRecorder()',1)[0]
        self.assertLess(stop.index('m->worker.join()'),stop.index('ReleaseLocalRecorder(m->controlLease)'))
        self.assertNotIn('claimed',owner)
        self.assertIn('ClaimLocalRecorder(owner->m->controlLease)',owner)
        worker=owner.split('void FrameHistory::runRecorder()',1)[1].split('FrameHistory::Capture',1)[0]
        self.assertEqual(worker.count('&m->controlLease'),5)
        self.assertNotIn('},true',worker)
        control=owner.split('json FrameHistoryControl(',1)[1]
        self.assertLess(control.index('OwnsLocalRecorder(*localLease)'),control.index('registryLock(r.mutex)'))
        self.assertIn('localLease!=&m.controlLease',control)
        self.assertIn('localLease&&m.stopping.load()',control)
    def test_launcher_no_required_python_and_honest_hud(self):
        s=read('AutoTest/launch_frame_history_player.ps1')
        self.assertIn('$python=if($ExternalWatcher)',s)
        self.assertIn("DXVK_WAR3_FRAME_HISTORY_SELF_CONTAINED=if($ExternalWatcher){'0'}else{'1'}",s)
        ui=read('src/d3d9/war3/ui/war3_imgui.cpp')
        panel=ui.split('void War3Imgui::drawFrameRecorderPanel()',1)[1].split('void War3Imgui::drawDebugWindow()',1)[0]
        self.assertRegex(panel,r'if\s*\(\s*recorder\.selfContained\s*\)')
        self.assertIn('完整性待离线分析',panel)
    def test_native_contracts_registered(self):
        s=read('src/d3d9/meson.build')
        for name in ('war3_frame_recorder_session','war3_frame_evidence_runtime','war3_frame_evidence_disabled'):
            self.assertIn("test('"+name+"'",s)
    def test_image_worker_state_has_one_numeric_authority(self):
        owner=read('src/d3d9/war3/tools/war3_frame_history.cpp')
        session=read('src/d3d9/war3/tools/war3_frame_recorder_session.h')
        self.assertIn('struct FrameHistory::Impl : history::CaptureLifecycle',owner)
        self.assertNotIn('enum State:',owner)
        self.assertIn('CaptureLifecycle::State imageState',session)
        self.assertIn('imageState == CaptureLifecycle::Complete',session)
        self.assertIn('imageState == CaptureLifecycle::Fault',session)
        self.assertNotIn('imageState == 6',session)
        self.assertNotIn('imageState == 7',session)
        self.assertIn('m->state.load(),',owner.split('session.next(',1)[1].split(';',1)[0])

if __name__=='__main__':unittest.main()
