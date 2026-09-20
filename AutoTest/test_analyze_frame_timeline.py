import copy, unittest
from unittest.mock import patch
from pathlib import Path
import analyze_frame_timeline as subject

def fixture():
 r={'start':'100','end':'200','faults':0,'result':0,'self':[25,75],'inclusive':[0,75],
    'legacySelf':[20,40],'legacyTicks':60,'calls':[0,1],'cpu100ns':30}
 return {'mainThreadTimeline':{'enabled':True,'otherPresents':0,'frequency':10000,'threadId':5,
          'buckets':['OutsideScopes','Known'],'frames':[r]},'frameCount':1,'meta':{}}

class Analysis(unittest.TestCase):
 def run_data(self,data,duplicates=None):
  with patch.object(subject,'load',return_value=(data,duplicates or [],'testhash')):
   return subject.analyze(Path('synthetic.html'))
 def test_exact_closure(self):
  r=self.run_data(fixture());self.assertEqual(r['fullPresentIntervalMs'],10)
  self.assertEqual(r['legacyWindowIntersectionMs'],6);self.assertEqual(r['buckets'][1]['selfMs'],2.5)
  self.assertFalse(r['productAccepted'])
 def test_duplicate_root_fails(self):
  with self.assertRaises(ValueError):self.run_data(fixture(),[{'path':'x'}])
 def test_clock_fault_and_present_failure(self):
  for field,value in [('faults',1),('result',-1),('end','100')]:
   d=fixture();d['mainThreadTimeline']['frames'][0][field]=value
   with self.assertRaises(ValueError):self.run_data(d)
 def test_self_or_legacy_gap_fails(self):
  for field in ['self','legacySelf']:
   d=fixture();d['mainThreadTimeline']['frames'][0][field][0]+=1
   with self.assertRaises(ValueError):self.run_data(d)
 def test_negative_and_inclusive_overflow_fail(self):
  for field,value in [('calls',-1),('inclusive',101)]:
   d=fixture();d['mainThreadTimeline']['frames'][0][field][1]=value
   with self.assertRaises(ValueError):self.run_data(d)
 def test_discontinuous_and_cross_owner_fail(self):
  d=fixture();r=copy.deepcopy(d['mainThreadTimeline']['frames'][0]);r['start']='201';r['end']='301'
  d['mainThreadTimeline']['frames'].append(r)
  with self.assertRaises(ValueError):self.run_data(d)
  d=fixture();d['mainThreadTimeline']['otherPresents']=1
  with self.assertRaises(ValueError):self.run_data(d)

if __name__=='__main__':unittest.main()
