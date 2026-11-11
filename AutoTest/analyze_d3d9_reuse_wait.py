"""Strict opt-in CPU wait attribution; no inference of pure GPU execution time."""
import math
import statistics


COUNTERS = ('episodes', 'gpuCalls', 'totalNs', 'cpuSubmissionNs', 'gpuWaitNs', 'retirementNs')


def analyze(rows, fixture, *, groups):
    def require(value, reason):
        if not value:
            raise ValueError(reason)

    require(type(rows) is list and 44 <= len(rows) <= 128, 'missing/bounded wait samples')
    fields = {'schema', 'query', 'draws', 'dropped', *COUNTERS}
    previous = dict.fromkeys(('query', 'draws', *COUNTERS), 0)
    frames = []
    draws = fixture['drawsPerFrame']
    require(type(draws) is int and draws > 0 and fixture['frames'] == 44 and
            fixture['warmFrames'] == 4, 'exact fixture frame population')
    batches = math.ceil(draws / 32)
    expected = (batches - 1) // 4 if groups else max(0, batches - 4)
    for index, row in enumerate(rows):
        require(type(row) is dict and set(row) == fields and
                row['schema'] == 'warvk-d3d9-reuse-wait/v1', 'wait record schema')
        require(all(type(row[key]) is int and 0 <= row[key] < 1 << 64
                    for key in fields - {'schema'}), 'unsigned exact wait counters')
        require(row['dropped'] == 0, 'wait samples dropped')
        require(row['query'] == previous['query'] + 1 and
                all(row[key] >= previous[key] for key in ('draws', *COUNTERS)),
                'nonmonotonic/missing wait interval')
        delta = {key: row[key] - previous[key] for key in COUNTERS}
        require(sum(delta[key] for key in ('cpuSubmissionNs', 'gpuWaitNs', 'retirementNs')) <=
                delta['totalNs'], 'exclusive stage times exceed containing reuse time')
        require(delta['gpuCalls'] <= delta['episodes'], 'GPU waits exceed reuse episodes')
        if index < 44:
            require(row['draws'] - previous['draws'] == draws, 'query interval is not a workload frame')
            require(delta['episodes'] == expected and delta['gpuCalls'] == expected,
                    'actual reuse calls differ from workload/flight count')
            if index >= 4:
                frames.append(delta)
        previous = row
    require(len(fixture['completedTicks']) == 40 and type(fixture['qpcFrequency']) is int and
            fixture['qpcFrequency'] > 0, 'paired client timing missing')
    completed = [value * 1e9 / fixture['qpcFrequency'] for value in fixture['completedTicks']]
    require(all(type(value) is int and value > 0 for value in fixture['completedTicks']), 'client ticks')
    require(all(row['totalNs'] <= end for row, end in zip(frames, completed)),
            'reuse duration exceeds matching complete frame')
    return dict(scope='40_FRAME_EVENT_INTERVALS_CPU_WALL_NOT_GPU_EXECUTION',
                expectedReuseEpisodesPerFrame=expected, frames=frames,
                mediansMs={key: statistics.median(row[key] / 1e6 for row in frames)
                           for key in COUNTERS if key.endswith('Ns')},
                totalReuseFraction=sum(row['totalNs'] for row in frames) / sum(completed),
                gpuWaitCallFraction=sum(row['gpuWaitNs'] for row in frames) / sum(completed),
                sampleCount=40, includesFinalEventWait=False, instrumentationEnabled=True,
                performanceAcceptance=False)
