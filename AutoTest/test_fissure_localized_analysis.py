import unittest
from investigate_history_fissures import draw_parts,draw_key
from investigate_fissure_caster_identity import signature,rawcode
from summarize_fissure_route_lifetimes import runs

def event():
    return {'data':['84','0','1','2','0','624','3','0','210','0','0',str(int.from_bytes(b'hfoo','big'))],
            'words32':[0]*48}
class LocalizedAnalysisTests(unittest.TestCase):
    def test_runs_exact(self):self.assertEqual(runs([69,70,71,103,105,106,104]),[[69,71],[103,106]])
    def test_runs_no_duplicate_duration(self):self.assertEqual(runs([69,69,70]),[[69,70]])
    def test_empty_runs(self):self.assertEqual(runs([]),[])
    def test_rawcode(self):self.assertEqual(rawcode(int.from_bytes(b'hfoo','big')),'hfoo')
    def test_draw_index_is_not_identity_signature(self):
        a=event();b=event();b['data'][0]='99'
        self.assertEqual(signature(a),signature(b));self.assertNotEqual(draw_key(a),draw_key(b))
    def test_handle_is_in_identity(self):
        a=event();b=event();a['words32'][26]=123;b['words32'][26]=456
        self.assertNotEqual(signature(a),signature(b))
    def test_matrix_is_light_vp_not_object_pose(self):
        parts=draw_parts(event());self.assertIn('lightVp',parts);self.assertNotIn('pose',parts)
    def test_alpha_flag_change_visible(self):
        a=event();b=event();b['words32'][16]=4
        self.assertNotEqual(draw_parts(a)['alpha'],draw_parts(b)['alpha'])
if __name__=='__main__':unittest.main()
