"""Strict receipt counter/deadline reader tests; these are not native GPU proof."""
import copy
import unittest

from run_d3d9_draw_batch_gate import ROOT, check_owner_metrics, check_event_metrics, timing, check_client_refresh, cpu_cost


class ReaderTests(unittest.TestCase):
    def test_event_completion_requires_exact_remote_gpu_proof(self):
        ps=dict(asyncEvents=True,eventWaitsPublished=44,eventWaitsSucceeded=44,eventPendingObservations=5,
                eventLocalCompletions=49,eventGpuCompleted=200)
        hs=dict(queryIssues=49,queryPolls=0,queryWaits=44,gpuCompleted=201)
        check_event_metrics(ps,hs,True)
        for patch in ({'eventWaitsSucceeded':43},{'eventWaitsPublished':50,'eventWaitsSucceeded':50},
                      {'eventLocalCompletions':0},{'eventGpuCompleted':202},{'eventWaitsPublished':True},
                      {'asyncEvents':False},{'eventPendingObservations':-1}):
            with self.subTest(patch=patch),self.assertRaises(ValueError):check_event_metrics(ps|patch,hs,True)
        for patch in ({'queryPolls':1},{'queryWaits':43},{'gpuCompleted':True},{'queryIssues':0}):
            with self.subTest(patch=patch),self.assertRaises(ValueError):check_event_metrics(ps,hs|patch,True)
        with self.assertRaises(ValueError):check_event_metrics(ps,hs,False)
        for key in ps:
            bad=ps.copy();del bad[key]
            with self.subTest(missing=key),self.assertRaises((ValueError,KeyError)):check_event_metrics(bad,hs,True)

    def test_only_proven_client_drift_is_allowed(self):
        client=[dict(path=str(ROOT/'tools/render_host/d3d9_connection.h'))]
        deps='object: #deps 3, deps mtime 42 (VALID)\nd3d9_backend.cpp\nd3d9_host_main.cpp\ndxvk_context.cpp\n'
        check_client_refresh(client,deps)
        for changes,text in (([],deps),(client*2,deps),(client,deps+'../d3d9_connection.h'),
                              (client,''),([dict(path=str(ROOT/'tools/render_host/d3d9_wire.h'))],deps)):
            with self.subTest(changes=changes,text=text),self.assertRaises(ValueError):
                check_client_refresh(changes,text)

    def records(self):
        bs=dict(schema='warvk-d3d9-draw-batch-host/v1',batches=5,batchedDraws=128,
                largestBatch=32,constantCapacityBytes=1048576,draws=129,submitted=8,
                gpuCompleted=8,retired=8,flightCapacity=4,flightSubmissions=5,
                flightReuses=1,flightRetirements=5,maxOccupiedFlightSlots=4,
                submissionGroupCapacity=1,groupedSubmissions=0,nonIndexedDraws=0,
                batchStateReuse=0,redundantStateBindingsSkipped=0)
        hs=dict(ok=True,**{k:bs[k] for k in ('draws','submitted','gpuCompleted','retired')})
        return bs,hs

    def test_closed_owner(self):
        check_owner_metrics(*self.records(),True)

    def test_state_reuse_cannot_skip_first_draw(self):
        bs,hs=self.records();bs.update(batchStateReuse=1,redundantStateBindingsSkipped=868)
        check_owner_metrics(bs,hs,True,False,True)
        for patch in ({'batchStateReuse':0},{'redundantStateBindingsSkipped':869},{'redundantStateBindingsSkipped':0}):
            with self.subTest(patch=patch),self.assertRaises(ValueError):check_owner_metrics(bs|patch,hs,True,False,True)
        with self.assertRaises(ValueError):check_owner_metrics(bs,hs,True)

    def test_groups_require_bounded_complete_owner(self):
        bs,hs=self.records();bs.update(submissionGroupCapacity=4,groupedSubmissions=2,nonIndexedDraws=32)
        check_owner_metrics(bs,hs,True,True)
        for patch in ({'groupedSubmissions':0},{'groupedSubmissions':1},{'groupedSubmissions':6},
                      {'submissionGroupCapacity':1},{'nonIndexedDraws':130}):
            with self.subTest(patch=patch),self.assertRaises(ValueError):check_owner_metrics(bs|patch,hs,True,True)
        with self.assertRaises(ValueError):check_owner_metrics(bs,hs,True,False)

    def test_cpu_scope_and_counters(self):
        row=dict(qpcFrequency=1000,workloadWallTicks=200,parentKernel100ns=100,parentUser100ns=200,
                 hostKernel100ns=300,hostUser100ns=400,systemKernel100ns=1000,systemUser100ns=2000,
                 systemIdle100ns=500,parentCpuCycles=1000,hostCpuCycles=2000,cpuIncludesReadback=True,hostCpuMeasured=True)
        self.assertAlmostEqual(cpu_cost(row,False)['totalCpuMs'],.1)
        for key in set(row)-{'cpuIncludesReadback','hostCpuMeasured'}:
            for value in (True,-1,'100',1<<64):
                with self.subTest(key=key,value=value),self.assertRaises(ValueError):cpu_cost(row|{key:value},False)
        for patch in ({'workloadWallTicks':0},{'systemIdle100ns':1001},{'cpuIncludesReadback':False},
                      {'hostCpuMeasured':False},{'qpcFrequency':0},{'parentCpuCycles':0},{'hostCpuCycles':0}):
            with self.subTest(patch=patch),self.assertRaises(ValueError):cpu_cost(row|patch,False)
        with self.assertRaises(ValueError):cpu_cost(row|{'hostCpuMeasured':False},True)
        cpu_cost(row|{'hostCpuMeasured':False,'hostKernel100ns':0,'hostUser100ns':0,'hostCpuCycles':0},True)
        missing=cpu_cost(row|{'systemKernel100ns':0,'systemUser100ns':0,'systemIdle100ns':0},False)
        self.assertIsNone(missing['systemBusyPercent']);self.assertFalse(missing['systemCpuAvailable'])
        self.assertEqual(missing['systemCpuUnavailableReason'],'counterDidNotAdvanceInShortWindow')

    def test_missing_or_forged_counters(self):
        original,host=self.records()
        for key in original:
            with self.subTest(missing=key),self.assertRaises((ValueError,KeyError)):
                bs=copy.deepcopy(original);del bs[key];check_owner_metrics(bs,host,True)
        for key in set(original)-{'schema'}:
            for value in (True,False,-1,1<<64,'1'):
                with self.subTest(key=key,value=value),self.assertRaises(ValueError):
                    bs=copy.deepcopy(original);bs[key]=value;check_owner_metrics(bs,host,True)

    def test_gpu_and_cpu_milestones_are_not_interchangeable(self):
        for patch in ({'retired':7},{'gpuCompleted':7},{'flightRetirements':4},
                      {'flightCapacity':5},{'maxOccupiedFlightSlots':5},{'flightReuses':6},
                      {'constantCapacityBytes':1048577},{'batchedDraws':130},{'largestBatch':33}):
            with self.subTest(patch=patch),self.assertRaises(ValueError):
                bs,hs=self.records();bs.update(patch);check_owner_metrics(bs,hs,True)
        bs,hs=self.records();hs['ok']=False
        with self.assertRaises(ValueError):check_owner_metrics(bs,hs,True)
        with self.assertRaises(ValueError):check_owner_metrics(*self.records(),False)

    def test_timing_window_requires_complete_samples(self):
        good=dict(qpcFrequency=1000,admissionTicks=[1]*40,completedTicks=[2]*40)
        self.assertEqual(timing(good)['completedTicks']['medianMs'],2)
        for patch in ({'qpcFrequency':True},{'qpcFrequency':0},{'admissionTicks':[1]*39},
                      {'admissionTicks':[3]*40},{'completedTicks':[False]*40},
                      {'completedTicks':[0]*40}):
            with self.subTest(patch=patch),self.assertRaises(ValueError):timing(good|patch)


if __name__=='__main__':unittest.main()
