import json
from pathlib import Path
import tempfile
import unittest
from analyze_data_collection_perf_report import load, audit_tree, ticks

class ReportTests(unittest.TestCase):
 def test_duplicates_retained_not_accepted(self):
  with tempfile.TemporaryDirectory() as directory:
   p=Path(directory)/'a.html';p.write_text('const data = {"a":1,"a":2,"nested":{"x":3,"x":3}};',encoding='utf-8')
   d,dup,_=load(p)
   self.assertEqual(d['a'],{'duplicateValues':[1,2]})
   self.assertEqual(d['nested']['x'],{'duplicateValues':[3,3]})
   self.assertEqual(len(dup),2)
 def tree(self):
  return {'enabled':True,'coverageComplete':False,'frames':99,'samplePeriod':64,'threads':[
   {'threadId':7,'mainThread':True,'closed':True,'faults':0,'inFlight':False,'nodes':[
    {'id':0,'parent':0,'name':'DataCollection','ticks':'100','childTicks':'100','inclusiveMs':1,'unclassifiedMs':0,'sampledCalls':1},
    {'id':1,'parent':0,'name':'DrawCapture','ticks':'100','childTicks':'0','inclusiveMs':1,'unclassifiedMs':1,'sampledCalls':1}]}]}
 def test_integer_closure_and_no_frame_rate_conversion(self):
  result=audit_tree(self.tree());t=result['threads'][0]
  self.assertEqual(t['closureErrors'],[])
  self.assertEqual(t['sampledRootMs'],1)
  self.assertFalse(result['coverageComplete'])
  self.assertEqual(t['topLevel'][0]['shareOfSampledDataRootPct'],100)
 def test_false_reported_closure_detected(self):
  tree=self.tree();tree['threads'][0]['nodes'][0]['childTicks']='99'
  self.assertTrue(audit_tree(tree)['threads'][0]['closureErrors'])
 def test_invalid_parent_and_ticks(self):
  tree=self.tree();tree['threads'][0]['nodes'][1]['parent']=1
  with self.assertRaises(ValueError):audit_tree(tree)
  for value in [1,True,'NaN','-1',str(2**64)]:
   with self.subTest(value=value),self.assertRaises(ValueError):ticks(value)
 def test_missing_disabled_tree(self):
  self.assertFalse(audit_tree(None)['present'])
  self.assertFalse(audit_tree({'enabled':False})['enabled'])
 def test_nonfinite_json_is_rejected(self):
  with tempfile.TemporaryDirectory() as directory:
   p=Path(directory)/'a.html';p.write_text('const data = {"x":NaN};',encoding='utf-8')
   with self.assertRaises(ValueError):load(p)

if __name__=='__main__':unittest.main()
