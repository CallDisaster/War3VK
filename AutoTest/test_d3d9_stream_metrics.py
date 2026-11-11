import copy
import unittest
from run_d3d9_draw_batch_gate import STREAM_FIELDS, check_owner_metrics, check_stream_metrics


class StreamMetricReaderTests(unittest.TestCase):
    def owner(self):
        draws = 44*1025+193
        bs = dict(schema='warvk-d3d9-draw-batch-host/v1', batches=44*33+7, batchedDraws=draws, largestBatch=32,
                  constantCapacityBytes=4*1048576-4096, draws=draws, submitted=300, gpuCompleted=300, retired=300,
                  flightCapacity=1, flightSubmissions=0, flightReuses=0, flightRetirements=0,
                  maxOccupiedFlightSlots=0, submissionGroupCapacity=1, groupedSubmissions=0,
                  nonIndexedDraws=32, batchStateReuse=0, redundantStateBindingsSkipped=0)
        hs = dict(ok=True, draws=draws, submitted=300, gpuCompleted=300, retired=300)
        ss = dict(schema='warvk-d3d9-stream-host/v1', streamDraws=draws, streamFixedDraws=0,
                  streamSubmissions=52, streamCapSubmissions=0, largestStreamList=1025,
                  maxStreamDraws=4096, maxStreamLatency=3, constantRenames=190, latencyWaits=0,
                  ringBytes=bs['constantCapacityBytes'], submitted=300, gpuCompleted=300, retired=300)
        return bs, hs, ss

    def test_real_stream_shape_accepted(self):
        bs, hs, ss = self.owner()
        check_owner_metrics(bs, hs, False, stream=True)
        self.assertEqual(check_stream_metrics([ss], bs, True), ss)
        self.assertIsNone(check_stream_metrics([], bs, False))

    def test_ring_bound_only_widens_for_stream(self):
        bs, hs, _ = self.owner()
        with self.assertRaises(ValueError):
            check_owner_metrics(bs, hs, False)
        bs['constantCapacityBytes'] = (4 << 20)+1
        with self.assertRaises(ValueError):
            check_owner_metrics(bs, hs, False, stream=True)

    def test_missing_damaged_or_unexpected_stream_rows_rejected(self):
        bs, _, ss = self.owner()
        for rows in ([], [ss, ss]):
            with self.assertRaises(ValueError):
                check_stream_metrics(rows, bs, True)
        with self.assertRaises(ValueError):
            check_stream_metrics([ss], bs, False)
        for key, value in (('schema', 'old'), ('streamDraws', 1), ('streamFixedDraws', 10**9),
                           ('maxStreamDraws', 1024), ('maxStreamLatency', 0), ('largestStreamList', 0),
                           ('largestStreamList', 4097), ('streamSubmissions', 0), ('streamSubmissions', 301),
                           ('streamCapSubmissions', 53), ('ringBytes', 1), ('submitted', 299),
                           ('retired', 301), ('constantRenames', True), ('latencyWaits', -1)):
            damaged = copy.deepcopy(ss)
            damaged[key] = value
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                check_stream_metrics([damaged], bs, True)
        for key in STREAM_FIELDS:
            damaged = copy.deepcopy(ss)
            del damaged[key]
            with self.subTest(missing=key), self.assertRaises(ValueError):
                check_stream_metrics([damaged], bs, True)


if __name__ == '__main__':
    unittest.main()
