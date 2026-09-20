import json
import unittest
from run_render_host_producer_inbox_gate import load_result, validate


def valid():
    return dict(ok=True,bits=32,checks=120000,queueBytes=262400,sampleBytes=65584,
                atomicBytes=4,lockFree=True,pausedAttempts=2000,pausedPublished=4,pausedFull=1996,
                concurrentAttempts=50000,concurrentPublished=5000,concurrentFull=45000,
                concurrentBytes=65536000,cppAllocationForbiddenInQueueCalls=True,
                consumerStopDrained=True,freshScopeRecovery=True,gpuTested=False,ipcTested=False)


class Acceptance(unittest.TestCase):
    def test_positive_both_widths(self):
        for bits in (32,64):
            r=valid();r['bits']=bits;validate(load_result(json.dumps(r)),bits)

    def test_wrong_counter_types_and_ranges(self):
        for key in ('checks','queueBytes','atomicBytes','concurrentFull','concurrentBytes'):
            for value in (True,1.0,-1,float('nan'),'4'):
                with self.subTest(key=key,value=value),self.assertRaises(ValueError):
                    r=valid();r[key]=value;validate(r,32)

    def test_population_cannot_hide_drops_or_no_positive_samples(self):
        for key,value in (('pausedPublished',5),('pausedFull',0),('pausedAttempts',1999),
                          ('concurrentFull',45001),('concurrentPublished',0),('concurrentAttempts',0),
                          ('concurrentBytes',0),('concurrentBytes',65536000000)):
            with self.subTest(key=key),self.assertRaises(ValueError):
                r=valid();r[key]=value;validate(r,32)

    def test_footprint_and_operating_boundaries(self):
        for key,value in (('queueBytes',400000),('sampleBytes',1),('atomicBytes',8),('bits',64),
                          ('lockFree',False),('cppAllocationForbiddenInQueueCalls',False),
                          ('consumerStopDrained',False),('freshScopeRecovery',False),('gpuTested',True),('ipcTested',True),('ok',False)):
            with self.subTest(key=key),self.assertRaises(ValueError):
                r=valid();r[key]=value;validate(r,32)

    def test_strict_root_duplicate_and_nonfinite(self):
        for raw in ('[]','null','{"ok":false,"ok":true}','{"x":{"a":1,"a":2}}',
                    '{"x":NaN}','{"x":Infinity}'):
            with self.subTest(raw=raw),self.assertRaises(ValueError):load_result(raw)

    def test_missing_counter_not_defaulted_to_zero(self):
        for key in valid():
            with self.subTest(key=key),self.assertRaises(ValueError):
                r=valid();del r[key];validate(r,32)


if __name__=='__main__':unittest.main()
