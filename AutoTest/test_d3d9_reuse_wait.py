import copy
import unittest
from analyze_d3d9_reuse_wait import analyze


class WaitReaderTests(unittest.TestCase):
    def records(self, groups=True):
        count = 8 if groups else 29
        rows = [dict(schema='warvk-d3d9-reuse-wait/v1', query=i, draws=1025*i,
                     episodes=count*i, gpuCalls=count*i, totalNs=1000*i,
                     cpuSubmissionNs=100*i, gpuWaitNs=600*i, retirementNs=200*i, dropped=0)
                for i in range(1, 45)]
        fixture = dict(drawsPerFrame=1025, frames=44, warmFrames=4,
                       completedTicks=[2000]*40, qpcFrequency=1000000000)
        return rows, fixture

    def test_two_real_workload_shapes(self):
        for groups in (False, True):
            result = analyze(*self.records(groups), groups=groups)
            self.assertEqual(result['sampleCount'], 40)
            self.assertEqual(result['totalReuseFraction'], .5)
            self.assertEqual(result['gpuWaitCallFraction'], .3)

    def test_missing_truncated_reordered_or_misattributed_rejected(self):
        rows, fixture = self.records()
        for key, value in (('query', 40), ('draws', 1), ('dropped', 1), ('episodes', 0),
                           ('gpuCalls', True), ('totalNs', 1), ('gpuWaitNs', 100000000),
                           ('schema', 'old'), ('retirementNs', -1)):
            damaged = copy.deepcopy(rows)
            damaged[8][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                analyze(damaged, fixture, groups=True)
        for damaged in ([], rows[:-1], rows[::-1], rows*4):
            with self.assertRaises(ValueError):
                analyze(damaged, fixture, groups=True)

    def test_clock_population_and_false_frame_proof_rejected(self):
        rows, fixture = self.records()
        for patch in (dict(frames=40), dict(warmFrames=0), dict(drawsPerFrame=97),
                      dict(completedTicks=[2000]*39), dict(completedTicks=[1]*40),
                      dict(qpcFrequency=0), dict(completedTicks=[True]*40)):
            with self.subTest(patch=patch), self.assertRaises(ValueError):
                analyze(rows, fixture | patch, groups=True)


if __name__ == '__main__':
    unittest.main()
