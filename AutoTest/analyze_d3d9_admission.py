"""Strict opt-in admission attribution per EVENT interval; CPU wall time only.

Host, IPC worker and x86 producer counters are each cut after an accepted
Issue. Nested stages are checked; the derived pipe share is a difference of
two clocks on one machine, not a measured transfer time or GPU execution.
"""
import math
import statistics


HOST = ('messages', 'idleNs', 'readNs', 'replyNs', 'batchMessages', 'batchDraws', 'batchReadNs',
        'batchDecodeNs', 'batchApplyNs', 'batchReplyNs', 'prepareNs', 'recordNs', 'constantNs',
        'bindNs', 'samplerNs', 'drawNs', 'flushApplyNs', 'waitEventApplyNs', 'otherApplyNs')
WORKER = ('messages', 'idleNs', 'writeNs', 'replyNs', 'batchMessages', 'batchWriteNs', 'batchReplyNs')
PRODUCER = ('publishes', 'backpressureEpisodes', 'backpressureNs', 'calls', 'callWaitNs')


def require(value, reason):
    if not value:
        raise ValueError(reason)


def intervals(rows, schema, counters):
    require(type(rows) is list and 44 <= len(rows) <= 128, schema + ' bounded samples')
    fields = {'schema', 'query', 'dropped', *counters}
    previous = dict.fromkeys(counters, 0)
    result = []
    for index, row in enumerate(rows):
        require(type(row) is dict and set(row) == fields and row['schema'] == schema, schema + ' record schema')
        require(all(type(row[key]) is int and 0 <= row[key] < 1 << 64 for key in fields - {'schema'}),
                schema + ' unsigned counters')
        require(row['dropped'] == 0, schema + ' samples dropped')
        require(row['query'] == index + 1, schema + ' missing or reordered interval')
        require(all(row[key] >= previous[key] for key in counters), schema + ' nonmonotonic counters')
        result.append({key: row[key] - previous[key] for key in counters})
        previous = row
    return result[:44]


def analyze(host, worker, producer, fixture, capacity=32):
    # 32 programmable or 20 fixed-function draws per batch message.
    require(capacity in (20, 32), 'known batch capacity')
    draws = fixture['drawsPerFrame']
    require(type(draws) is int and draws > 0 and fixture['frames'] == 44 and fixture['warmFrames'] == 4,
            'exact fixture frame population')
    frequency = fixture['qpcFrequency']
    completed, admitted = fixture['completedTicks'], fixture['admissionTicks']
    require(type(frequency) is int and frequency > 0 and type(completed) is list and type(admitted) is list
            and len(completed) == len(admitted) == 40
            and all(type(v) is int and v > 0 for v in completed + admitted), 'paired client timing missing')
    h = intervals(host, 'warvk-d3d9-admission-host/v1', HOST)
    w = intervals(worker, 'warvk-d3d9-admission-worker/v1', WORKER)
    p = intervals(producer, 'warvk-d3d9-admission-producer/v1', PRODUCER)
    batches = math.ceil(draws / capacity)
    frames = []
    for index, (hh, ww, pp) in enumerate(zip(h, w, p)):
        require(hh['batchDraws'] == draws and hh['batchMessages'] == ww['batchMessages'] == batches,
                'interval is not one workload frame')
        require(hh['batchReadNs'] <= hh['readNs'] and hh['batchReplyNs'] <= hh['replyNs'],
                'host batch stages exceed host totals')
        require(hh['batchDecodeNs'] + hh['prepareNs'] + hh['recordNs'] <= hh['batchApplyNs'],
                'nested batch stages exceed containing apply time')
        require(hh['constantNs'] + hh['bindNs'] + hh['drawNs'] <= hh['recordNs'],
                'recording stages exceed containing recording')
        require(hh['samplerNs'] <= hh['bindNs'], 'sampler binding exceeds containing state binding')
        require(ww['batchWriteNs'] <= ww['writeNs'] and ww['batchReplyNs'] <= ww['replyNs'],
                'worker batch stages exceed worker totals')
        require(pp['backpressureEpisodes'] <= pp['publishes'], 'backpressure without a publication')
        if index < 4:
            continue
        roundTrip = ww['batchWriteNs'] + ww['batchReplyNs']
        frames.append(dict(
            hostBatchReadNs=hh['batchReadNs'], hostDecodeNs=hh['batchDecodeNs'], hostPrepareNs=hh['prepareNs'],
            hostConstantNs=hh['constantNs'], hostSamplerNs=hh['samplerNs'],
            hostBindOtherNs=hh['bindNs'] - hh['samplerNs'], hostDrawNs=hh['drawNs'],
            # Recording outside constants, binding and the draw: ring slices,
            # stage setup and loop overhead.
            hostRecordOtherNs=hh['recordNs'] - hh['constantNs'] - hh['bindNs'] - hh['drawNs'],
            hostBatchDispatchNs=hh['batchApplyNs'] - hh['batchDecodeNs'] - hh['prepareNs'] - hh['recordNs'],
            hostBatchApplyNs=hh['batchApplyNs'], hostBatchReplyNs=hh['batchReplyNs'],
            hostFlushApplyNs=hh['flushApplyNs'], hostWaitEventApplyNs=hh['waitEventApplyNs'],
            hostOtherApplyNs=hh['otherApplyNs'], hostIdleNs=hh['idleNs'],
            workerBatchWriteNs=ww['batchWriteNs'], workerBatchReplyNs=ww['batchReplyNs'],
            workerBatchRoundTripNs=roundTrip, workerIdleNs=ww['idleNs'],
            # Round trip not spent inside host apply/reply: pipe copies, host
            # read and both wakeups. Clock-difference estimate, may be noisy.
            batchOutsideHostWorkNs=roundTrip - hh['batchApplyNs'] - hh['batchReplyNs'],
            producerBackpressureNs=pp['backpressureNs'], producerCallWaitNs=pp['callWaitNs'],
            producerCalls=pp['calls'], producerPublishes=pp['publishes']))
    completedNs = [v * 1e9 / frequency for v in completed]
    admittedNs = [v * 1e9 / frequency for v in admitted]
    keys = [k for k in frames[0] if k.endswith('Ns')]
    medians = {k[:-2] + 'Ms': statistics.median(f[k] / 1e6 for f in frames) for k in keys}
    medians['completedMs'] = statistics.median(v / 1e6 for v in completedNs)
    medians['admissionMs'] = statistics.median(v / 1e6 for v in admittedNs)
    total = sum(completedNs)
    shares = {k[:-2] + 'Share': sum(f[k] for f in frames) / total for k in keys}
    perBatch = {k[:-2] + 'UsPerBatch': statistics.median(f[k] / 1e3 / batches for f in frames)
                for k in keys if k.startswith(('hostBatch', 'hostDecode', 'hostPrepare', 'hostConstant',
                                               'hostSampler', 'hostBind', 'hostDraw', 'hostRecord',
                                               'workerBatch', 'batchOutside'))}
    return dict(scope='40_FRAME_EVENT_INTERVALS_CPU_WALL_NOT_GPU_EXECUTION', batchesPerFrame=batches, batchCapacity=capacity,
                drawsPerFrame=draws, mediansMs=medians, sharesOfCompleted=shares, perBatchMediansUs=perBatch,
                frames=frames, sampleCount=len(frames), instrumentationEnabled=True, performanceAcceptance=False)
