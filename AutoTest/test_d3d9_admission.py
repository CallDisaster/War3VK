import copy
import unittest
from analyze_d3d9_admission import HOST, PRODUCER, WORKER, analyze


class AdmissionReaderTests(unittest.TestCase):
    def records(self, count=47):
        def rows(schema, counters, per):
            return [dict(schema=schema, query=i, dropped=0, **{k: per[k]*i for k in counters})
                    for i in range(1, count+1)]
        host = dict.fromkeys(HOST, 0) | dict(messages=36, idleNs=200, readNs=300, replyNs=100, batchMessages=33,
                                             batchDraws=1025, batchReadNs=250, batchDecodeNs=50, batchApplyNs=900,
                                             batchReplyNs=90, prepareNs=200, recordNs=500, constantNs=300,
                                             bindNs=150, samplerNs=60, drawNs=40)
        worker = dict.fromkeys(WORKER, 0) | dict(messages=36, idleNs=10, writeNs=400, replyNs=1300,
                                                 batchMessages=33, batchWriteNs=380, batchReplyNs=1200)
        producer = dict(publishes=36, backpressureEpisodes=30, backpressureNs=1500, calls=2, callWaitNs=300)
        fixture = dict(drawsPerFrame=1025, frames=44, warmFrames=4, qpcFrequency=10**9,
                       completedTicks=[3000]*40, admissionTicks=[2800]*40)
        return (rows('warvk-d3d9-admission-host/v1', HOST, host),
                rows('warvk-d3d9-admission-worker/v1', WORKER, worker),
                rows('warvk-d3d9-admission-producer/v1', PRODUCER, producer), fixture)

    def test_real_workload_shape(self):
        result = analyze(*self.records())
        self.assertEqual(result['sampleCount'], 40)
        self.assertEqual(result['batchesPerFrame'], 33)
        self.assertEqual(result['mediansMs']['workerBatchRoundTripMs'], 1580/1e6)
        self.assertEqual(result['frames'][0]['batchOutsideHostWorkNs'], 1580-900-90)
        self.assertEqual(result['frames'][0]['hostBatchDispatchNs'], 900-50-200-500)
        self.assertEqual(result['frames'][0]['hostRecordOtherNs'], 500-300-150-40)
        self.assertEqual(result['frames'][0]['hostBindOtherNs'], 150-60)
        self.assertAlmostEqual(result['sharesOfCompleted']['producerBackpressureShare'], .5)

    def test_fixed_function_capacity(self):
        host, worker, producer, fixture = self.records()
        for rows in (host, worker):
            for i, row in enumerate(rows, 1):
                row['batchMessages'] = 52 * i
        result = analyze(host, worker, producer, fixture, capacity=20)
        self.assertEqual((result['batchesPerFrame'], result['batchCapacity']), (52, 20))
        with self.assertRaises(ValueError):
            analyze(host, worker, producer, fixture)
        with self.assertRaises(ValueError):
            analyze(host, worker, producer, fixture, capacity=16)

    def test_missing_reordered_dropped_or_damaged_rejected(self):
        host, worker, producer, fixture = self.records()
        for owner in range(3):
            for damage in ('drop', 'reorder', 'truncate', 'dropped', 'negative', 'schema', 'bool', 'extra'):
                rows = [copy.deepcopy(r) for r in (host, worker, producer)]
                target = rows[owner]
                if damage == 'drop':
                    del target[7]
                elif damage == 'reorder':
                    target[5], target[6] = target[6], target[5]
                elif damage == 'truncate':
                    del target[43:]
                elif damage == 'dropped':
                    target[9]['dropped'] = 1
                elif damage == 'negative':
                    key = next(k for k in target[9] if k.endswith('Ns'))
                    target[9][key] = target[8][key]-1
                elif damage == 'schema':
                    target[2]['schema'] = 'old'
                elif damage == 'bool':
                    target[2]['dropped'] = False
                else:
                    target[2]['unexpected'] = 1
                with self.subTest(owner=owner, damage=damage), self.assertRaises(ValueError):
                    analyze(*rows, fixture)

    def test_nesting_and_frame_population_violations_rejected(self):
        host, worker, producer, fixture = self.records()
        for owner, key, value in ((0, 'constantNs', 10**6), (0, 'samplerNs', 10**6), (0, 'drawNs', 10**6),
                                  (0, 'bindNs', 10**6), (0, 'batchDecodeNs', 10**6), (0, 'batchDraws', 10**6),
                                  (0, 'batchReadNs', 10**6), (1, 'batchWriteNs', 10**6), (1, 'batchMessages', 10**6),
                                  (2, 'backpressureEpisodes', 10**6)):
            rows = [copy.deepcopy(r) for r in (host, worker, producer)]
            for row in rows[owner][9:]:
                row[key] += value
            with self.subTest(key=key), self.assertRaises(ValueError):
                analyze(*rows, fixture)
        for patch in (dict(frames=40), dict(warmFrames=0), dict(drawsPerFrame=97), dict(qpcFrequency=0),
                      dict(completedTicks=[3000]*39), dict(admissionTicks=[True]*40)):
            with self.subTest(patch=patch), self.assertRaises(ValueError):
                analyze(host, worker, producer, fixture | patch)


if __name__ == '__main__':
    unittest.main()
