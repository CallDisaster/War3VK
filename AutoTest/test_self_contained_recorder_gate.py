import copy
import ast
from pathlib import Path
import tempfile
import unittest
from run_self_contained_recorder_gate import (incident_paths, use_build_defaults,
    RECORDER_ACTIVATION_VARS, BUILD_DEFAULT_VARS)

class IncidentBinding(unittest.TestCase):
    def test_build_default_run_removes_all_activation_and_output_overrides(self):
        original={k:'1' for k in RECORDER_ACTIVATION_VARS}
        original.update(DXVK_WAR3_FRAME_EVIDENCE_OUTPUT='somewhere',DXVK_WAR3_INTERNAL_TEST_API='1',DXVK_WAR3_SKIN_PALETTE_CONTRACT='1')
        result=use_build_defaults(original)
        for key in BUILD_DEFAULT_VARS+('DXVK_WAR3_FRAME_EVIDENCE_OUTPUT',):self.assertNotIn(key,result)
        self.assertEqual(result,dict(DXVK_WAR3_INTERNAL_TEST_API='1'))
        self.assertIn('DXVK_WAR3_FRAME_EVIDENCE',original)
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name)
        paths=[]
        for name in ('manifest.json','cpu.json','inputs.json','inputs.bin'):
            p=self.root/name;p.touch();paths.append(str(p))
        self.value=dict(schema=1,selfContained=True,processId=123,rawExportComplete=True,
            evidenceValidated=False,rootCauseReady=False,session='7',processNonce='99',
            history=dict(session='7',manifest=paths[0]),
            cpu=dict(session='7',processNonce='99',path=paths[1],inputs=dict(manifest=paths[2],binary=paths[3])))
    def test_exact(self):self.assertEqual(len(incident_paths(self.value,self.root,123)),4)
    def test_owner(self):
        with self.assertRaises(RuntimeError):incident_paths(self.value,self.root,124)
    def test_session(self):
        self.value['history']['session']='8'
        with self.assertRaises(RuntimeError):incident_paths(self.value,self.root,123)
    def test_nonce(self):
        self.value['cpu']['processNonce']='100'
        with self.assertRaises(RuntimeError):incident_paths(self.value,self.root,123)
    def test_missing(self):
        self.value['cpu']['path']=str(self.root/'missing')
        with self.assertRaises(RuntimeError):incident_paths(self.value,self.root,123)
    def test_outside(self):
        with self.assertRaises(RuntimeError):incident_paths(self.value,self.root/'subdir',123)
    def test_no_false_acceptance_or_partial(self):
        for key,value in (('rawExportComplete',False),('evidenceValidated',True),('rootCauseReady',True),('selfContained',False)):
            v=copy.deepcopy(self.value);v[key]=value
            with self.assertRaises(RuntimeError):incident_paths(v,self.root,123)
    def test_no_frame_control_or_watcher_launch(self):
        s=Path(__file__).with_name('run_self_contained_recorder_gate.py').read_text(encoding='utf-8')
        for text in ('subprocess.Popen','frame_history_watch.py','SwitchDesktop','SendInput'):
            self.assertTrue(text not in s, text)
        names=[node.func.id for node in ast.walk(ast.parse(s)) if isinstance(node,ast.Call) and isinstance(node.func,ast.Name)]
        self.assertNotIn('_request',names)
        self.assertIn("frame_history.test_shortcut",s)
        self.assertIn('use_isolated_desktop=True',s)
        self.assertIn('baseline_width=2560, baseline_height=1440',s)
        self.assertIn('force=False,avoid_foreground_switch=True',s)

if __name__=='__main__':unittest.main()
