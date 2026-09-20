import copy
import tempfile
from pathlib import Path
import unittest
from PIL import Image
from analyze_frame_history import analyze_history
from test_analyze_frame_evidence import fixture,event

class HistoryReaderTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.root=Path(self.tmp.name)
        self.cpu=fixture();self.cpu['schema']=3;self.cpu['events']=[]
        rows=[];seq=0
        for i in range(1,7):
            local=[]
            for kind in (3,12,4,16):
                seq+=1;e=event(seq,kind);e['frame']=str(99+i)
                if kind==4:e['parent']=str(seq-2)
                if kind==16:e['data'][:4]=[str(i),str(199+i),'4','4']
                local.append(e)
            self.cpu['events']+=local
            path=self.root/f'slot-{i}.tga';Image.new('RGB',(4,4),(20+i,30,40)).save(path)
            rows.append(dict(ordinal=str(i),present=str(199+i),owner='55',frame=str(99+i),
                             mapEpoch='3',deviceEpoch='2',qpc=str(i*100),targetValue='1',observedValue='1',
                             file=str(path),saved=True))
        self.cpu.update(accepted=str(seq),triggerSequence='16')
        self.h=dict(schema=1,state=6,session='1',captureComplete=False,rootCauseReady=False,failed=0,
                    lockDrops='0',retained=6,saved=6,preFrames=4,postFrames=2,triggerOrdinal='4',
                    width=4,height=4,qpcFrequency='1000',frames=rows)
    def tearDown(self):self.tmp.cleanup()
    def test_linked_six_frames(self):
        r=analyze_history(self.h,self.cpu);self.assertTrue(r['historyWindowLinked']);self.assertFalse(r['rootCauseReady'])
        self.assertEqual(len(r['frames']),6);self.assertAlmostEqual(r['retainedSeconds'],.5)
    def test_gap_rejected(self):
        self.h['frames'][3]['present']='209'
        with self.assertRaises(ValueError):analyze_history(self.h,self.cpu)
    def test_stale_target_rejected(self):
        self.h['frames'][3]['observedValue']='0'
        with self.assertRaises(ValueError):analyze_history(self.h,self.cpu)
    def test_copy_missing(self):
        self.cpu['events'][3]['kind']=8
        with self.assertRaises(ValueError):analyze_history(self.h,self.cpu)
    def test_shadow_missing(self):
        self.cpu['events'][1]['kind']=8
        with self.assertRaises(ValueError):analyze_history(self.h,self.cpu)
    def test_wrong_epoch(self):
        self.h['frames'][0]['mapEpoch']='5'
        with self.assertRaises(ValueError):analyze_history(self.h,self.cpu)
    def test_dimensions(self):
        self.h['width']=5
        with self.assertRaises(ValueError):analyze_history(self.h,self.cpu)
    def test_drop_reported(self):
        self.h['lockDrops']='1'
        with self.assertRaises(ValueError):analyze_history(self.h,self.cpu)
    def test_duplicate_file(self):
        self.h['frames'][1]['file']=self.h['frames'][0]['file']
        with self.assertRaises(ValueError):analyze_history(self.h,self.cpu)
    def test_wrong_session(self):
        self.h['session']='2'
        with self.assertRaises(ValueError):analyze_history(self.h,self.cpu)
    def timed(self,ms):
        self.h.update(schema=2,preMilliseconds=ms,retainedPreFrames=4,trimmedPreFrames=0,
                      preSpanTicks='300',durationSatisfied=True)
    def test_time_budget_exact_boundary(self):
        self.timed(300)
        self.assertEqual(analyze_history(self.h,self.cpu)['preTriggerSeconds'],.3)
    def test_time_budget_insufficient_rejected(self):
        self.timed(301)
        with self.assertRaises(ValueError):analyze_history(self.h,self.cpu)
    def test_forged_time_span_rejected(self):
        self.timed(300);self.h['preSpanTicks']='301'
        with self.assertRaises(ValueError):analyze_history(self.h,self.cpu)
if __name__=='__main__':unittest.main()
