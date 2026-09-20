import ast
import ctypes
from pathlib import Path
import unittest
import run_skin_palette_crash_diagnostic as d


class DiagnosticContract(unittest.TestCase):
    def test_sampler_native_abi(self):
        self.assertEqual((ctypes.sizeof(d.Counters),ctypes.sizeof(d.Region)),(80,48))

    def test_sampler_own_process_read_only(self):
        row=d.memory(-1,True)
        self.assertNotIn('memoryError',row)
        self.assertGreater(row['PrivateUsage'],0)
        scan=row['lower4GiB']
        self.assertEqual(scan['scannedEnd'],2**32)
        self.assertEqual(sum(scan[k] for k in ('free','reserved','committed')),2**32)

    def test_invalid_handle_is_not_zero_memory(self):
        self.assertIn('memoryError',d.memory(0))
        self.assertNotIn('PrivateUsage',d.memory(0))

    def test_pair_changes_only_recording(self):
        a=d.env_for('full','same',Path('E:/evidence'))
        b=d.env_for('off','same',Path('E:/evidence'))
        self.assertEqual({k for k in a if a[k]!=b[k]}, {
            'DXVK_WAR3_FRAME_EVIDENCE','DXVK_WAR3_FRAME_EVIDENCE_INPUTS','DXVK_WAR3_FRAME_EVIDENCE_DRAWS'})

    def test_candidate_proof_always_enabled(self):
        for mode in ('full','off'):
            env=d.env_for(mode,'same',Path('E:/evidence'))
            for suffix in ('SKIN_PALETTE_CONTRACT','RENDERABLE_PART_PALETTE_SNAPSHOT',
                'SEMANTIC_LIVE_PALETTE_REFRESH','SUBMIT_LIVE_POSE_REBUILD_ON_LAG',
                'SUBMIT_LIVE_POSE_REBUILD_EVERY_FRAME'):
                self.assertEqual(env['DXVK_WAR3_'+suffix],'1')

    def test_no_game_or_player_mutation_api(self):
        tree=ast.parse(Path(d.__file__).read_text(encoding='utf-8'))
        calls={node.func.attr for node in ast.walk(tree) if isinstance(node,ast.Call) and isinstance(node.func,ast.Attribute)}
        self.assertFalse(calls & {'_run_war3_input_plan','_post_war3_key_pulse','SendInput',
            'WriteProcessMemory','SuspendThread','DebugActiveProcess'})
        copies=[node for node in ast.walk(tree) if isinstance(node,ast.Call) and
            isinstance(node.func,ast.Attribute) and node.func.attr=='copyfile']
        self.assertEqual(len(copies),2)
        self.assertTrue(all(isinstance(n.args[1],ast.Name) and n.args[1].id=='live' for n in copies))


if __name__=='__main__':unittest.main()
