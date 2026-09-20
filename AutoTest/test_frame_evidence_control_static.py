from pathlib import Path
import unittest

ROOT=Path(__file__).resolve().parents[1]
SCRIPT=(ROOT/'AutoTest/frame_evidence_control.py').read_text(encoding='utf-8')
CPP=(ROOT/'src/d3d9/war3/tools/war3_frame_evidence.cpp').read_text(encoding='utf-8')
CORE=(ROOT/'src/d3d9/war3/tools/war3_frame_evidence_core.h').read_text(encoding='utf-8')
PIPE=(ROOT/'src/d3d9/d3d9_war3_pipeline.cpp').read_text(encoding='utf-8')
SWAP=(ROOT/'src/d3d9/d3d9_swapchain.cpp').read_text(encoding='utf-8')
class Contracts(unittest.TestCase):
    def test_control_actions_and_no_shell(self):
        for action in ('arm','status','trigger','freeze','export','discard'):
            self.assertIn("'"+action+"'",SCRIPT)
        self.assertNotIn('subprocess',SCRIPT);self.assertIn("'frame_evidence'",SCRIPT)
    def test_gate_is_default_off_and_create_new(self):
        self.assertIn('DXVK_WAR3_FRAME_EVIDENCE',CPP);self.assertIn('CREATE_NEW',CPP)
        self.assertNotIn('REPLACE_EXISTING',CPP);self.assertIn('frozen evidence retained',CPP)
    def test_strict_state_and_token(self):
        for text in (CORE,CPP):
            self.assertIn('State::Frozen',text);self.assertIn('session',text)
        self.assertIn('explicit frozen discard required before arm',CPP)
        self.assertIn('session mismatch',CPP)
    def test_no_hot_path_fs_json(self):
        self.assertNotIn('nlohmann',CORE);self.assertNotIn('CreateFile',PIPE);self.assertNotIn('CreateFile',SWAP)
    def test_identity_separate_from_ring_slot(self):
        self.assertIn('input.frameSerial',PIPE);self.assertIn('input.frameIndex',PIPE)
        self.assertIn('frameIndex; // ring slot',PIPE)
    def test_caster_metadata_bounded(self):
        self.assertIn('std::min)(input.scene.shadowCasters.size(), size_t(512))',PIPE)
        self.assertIn('actualCasterInputs',CPP)
    def test_caster_identity_not_hashed_together(self):
        self.assertIn('caster.data[1]=draw.rawcode',PIPE)
        self.assertIn('caster.data[6]=draw.shadowExactGeometryKeyHash',PIPE)
        self.assertNotIn('caster.data[1] |=',PIPE)
        self.assertIn('draw.indexInfo.offset,draw.indexInfo.size',PIPE)
    def test_distinct_typed_events(self):
        for name in ('CasterInput','CasterBinding','CasterOmitted','ShadowState','ScreenshotPrepared','ScreenshotSaved','ScreenshotCopyRecorded'):
            self.assertIn(name,CORE)
    def test_capture_token_retained_until_save(self):
        screenshot=(ROOT/'src/d3d9/war3/tools/war3_async_screenshot.cpp').read_text(encoding='utf-8')
        save=screenshot.split('bool save(const Slot& slot)',1)[1].split('void run()',1)[0]
        self.assertIn('Record(slot.evidenceSession,event)',save)
        self.assertNotIn('ActiveSession()',save)
        self.assertIn('slot.evidenceSession=evidence::ActiveSession()',screenshot)
        self.assertIn('cEvidenceSession=copy->evidenceSession',SWAP)
    def test_shadow_summary_on_common_run(self):
        shadow=(ROOT/'src/d3d9/d3d9_war3_shadow.cpp').read_text(encoding='utf-8')
        self.assertIn('state.kind=ev::Kind::ShadowState',shadow)
        self.assertIn('[&,evidenceSession]() noexcept',shadow)
        self.assertLess(shadow.index('reconciliation = {};'),shadow.index('state.kind=ev::Kind::ShadowState'))
    def test_actual_draw_state_is_recorded_before_real_draw(self):
        shadow=(ROOT/'src/d3d9/d3d9_war3_shadow.cpp').read_text(encoding='utf-8')
        block=shadow.split('static const bool recordActualDraws=',1)[1].split('cascadeTriangles +=',1)[0]
        self.assertIn('Kind::DirectionalDraw',block)
        self.assertIn('event.bits[16]=pc.flags',block)
        self.assertIn('&pc.alphaRef',block)
        self.assertIn('m_volumeSunRenderPathActive?1:0',block)
        self.assertIn('uint64_t(prep.alphaImageView)',block)
        self.assertLess(block.index('ev::Record(token,event)'),block.index('ctx->cmdDrawIndexed'))
    def test_export_streams_frozen_records(self):
        export=CPP.split('bool createNew(',1)[1].split('std::string utf8',1)[0]
        self.assertIn('pending.reserve(128*1024)',export)
        self.assertIn('pending.size()<64*1024||flush()',export)
        self.assertIn('ring.visitFrozen',export)
        self.assertIn('snapshot(false)',CPP)
        self.assertNotIn('push_back',export)
    def test_hot_producers_do_not_share_control_mutex(self):
        hot=CPP.split('uint64_t Record(',1)[1].split('bool RequestTriggerFromGame',1)[0]
        self.assertNotIn('mutex',hot);self.assertNotIn('lock(',hot)
        self.assertIn('cell.busy.test_and_set',CORE)
        self.assertIn('m_writers.fetch_add(1)',CORE)
        self.assertIn('cell.event.sequence>=sequence',CORE)
if __name__=='__main__':unittest.main()
