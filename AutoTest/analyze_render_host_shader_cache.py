"""Bind separate canonical-cache observations to the real M0 checkpoint stream.

No GPU run and no wire change. This proves reuse for the fixed repeated-bytecode
workload, NOT a bound on all unique shaders, physical VRAM or game memory savings.
"""
import argparse
import copy
import json
from pathlib import Path

from run_render_host_protocol_gate import identity
from run_render_host_transport_gate import require, strict_json
from run_render_host_d3d9_gate import json_lines, native_uint
from run_render_host_memory_gate import check_case, read_records, SUMMARY, SAMPLES, SEQUENCES
import run_render_host_memory_gate as memory
import run_render_host_memory_destroy_mutation as baseline_reader


PREFIX = 'D3D9 shader-cache: '
SCHEMA = 'warvk-d3d9-shader-cache/v1'
IDENTITY = ('pid','thread','creation','nonceLow','nonceHigh','sequence','phase','ordinal')
COUNTERS = ('requests','hits','compileAttempts','compilesSucceeded','registrations',
    'raceLosers','validationFailures','registrationFailures','readyEntries','failedEntries',
    'keyBytes','keyCapacityBytes','constantBytes','constantCapacityBytes')


def cache_rows(text):
    result = []
    for line in text.splitlines():
        if 'D3D9 shader-cache' not in line:
            continue
        require(line.startswith(PREFIX), 'malformed cache observation prefix')
        result.append(strict_json(line[len(PREFIX):]))
    return result


def check_cache(rows, windows, token_bytes):
    require(type(rows) is list and type(windows) is list, 'cache/M0 observation lists required')
    require(len(rows) == len(windows) == 8, 'eight cache observations and M0 checkpoints required')
    require(native_uint(token_bytes) and token_bytes > 0 and token_bytes % 4 == 0,
            'canonical token bytes must be a nonempty uint32 word span')
    cookies = None; storage = None; session = None
    for index, (row, window) in enumerate(zip(rows, windows)):
        require(type(row) is dict and set(row) == {'schema',*IDENTITY,*COUNTERS,
            'vertexCookies','pixelCookies','includesPipelineOrDriverBytes'}, 'exact cache field set')
        require(row['schema'] == SCHEMA and row['includesPipelineOrDriverBytes'] is False, 'cache scope claim')
        require(all(native_uint(row[k]) for k in (*IDENTITY,*COUNTERS)), 'unsigned cache evidence required')
        require(all(native_uint(row[k], 32) for k in ('pid','thread','phase','ordinal')),
                'native-width cache identity required')
        require(row['pid'] > 0 and row['thread'] > 0 and row['creation'] > 0
                and (row['nonceLow'] != 0 or row['nonceHigh'] != 0), 'empty cache session identity')
        require(type(window) is dict and type(window.get('host')) is dict,
                'M0 host checkpoint object required')
        host = window['host']
        require(all(native_uint(host[k], 32 if k in ('pid','thread','phase','ordinal') else 64)
                    for k in IDENTITY), 'unsigned native-width M0 identity required')
        require(all(row[k] == host[k] for k in IDENTITY), 'cache observation not bound to exact host checkpoint')
        # main() still invokes the full M0 reader. These exact phase/sequence
        # guards also keep standalone use from certifying reordered/fake phases.
        phase, ordinal = SAMPLES[index]
        require((row['phase'], row['ordinal'], row['sequence']) == (phase, ordinal, SEQUENCES[index])
                and all(type(window[k]) is int and window[k] == row[k]
                        for k in ('phase','ordinal','sequence')), 'exact ordered M0 cache checkpoint')
        current_session = tuple(row[k] for k in ('pid','thread','creation','nonceLow','nonceHigh'))
        if session is None: session = current_session
        require(current_session == session, 'cache session identity changed')
        initial = index == 0
        expected_requests = 0 if initial else 2*row['ordinal']
        require(row['requests'] == expected_requests and row['hits'] == max(0, expected_requests-2), 'repeated-bytecode request/hit population')
        require(all(row[k] == (0 if initial else 2) for k in ('compileAttempts','compilesSucceeded','registrations','readyEntries')),
                'same bytecode produced duplicate canonical modules')
        require(all(row[k] == 0 for k in ('raceLosers','validationFailures','registrationFailures','failedEntries')), 'unexpected cache failure/race')
        require(row['keyBytes'] == (0 if initial else token_bytes), 'exact canonical bytecode storage')
        require(row['keyCapacityBytes'] >= row['keyBytes'] and row['constantCapacityBytes'] >= row['constantBytes'], 'cache size exceeds actual capacity')
        # stats() counts vector capacity * sizeof(element), not allocator bins.
        # M0 check_summary separately binds sizeof(DxsoDefinedConstant) == 20.
        # Do not predict capacity == size; stable larger capacities are legal.
        require(row['keyCapacityBytes'] % 4 == 0 and row['constantBytes'] % 20 == 0
                and row['constantCapacityBytes'] % 20 == 0, 'cache vector byte units')
        live = row['phase'] in (2,3,5)
        selected = []
        for name in ('vertexCookies','pixelCookies'):
            values = row[name]
            require(type(values) is list and len(values) == 4 and all(native_uint(v) for v in values), 'exact typed cookie slots')
            nonzero = [v for v in values if v]
            require(len(nonzero) == int(live), 'live shader-cookie population')
            if live: selected.append(nonzero[0])
        if live:
            require(selected[0] != selected[1], 'different shader stages aliased')
            if cookies is None: cookies = selected
            require(selected == cookies, 'canonical cookie changed across recreation')
            constant_sizes = [host['cpu']['kinds'][k]['cpuSizeBytes']
                              for k in ('vertexShader','pixelShader')]
            require(host['cpu']['valid'] is True and all(native_uint(n) for n in constant_sizes),
                    'valid consumed metadata byte observations required')
            expected_constants = sum(constant_sizes)
            require(row['constantBytes'] == expected_constants, 'cached metadata is not the consumed metadata population')
        footprint = tuple(row[k] for k in ('keyBytes','keyCapacityBytes','constantBytes','constantCapacityBytes'))
        if initial:
            require(footprint == (0,0,0,0), 'device-ready cache not empty')
        else:
            if storage is None: storage = footprint
            require(footprint == storage, 'same canonical module storage grew')
    return dict(schema='warvk-shader-cache-reuse-analysis/v1', ok=True, checkpoints=8,
        createRequests=rows[-1]['requests'], compilesSucceeded=rows[-1]['compilesSucceeded'],
        cacheHits=rows[-1]['hits'], registrations=rows[-1]['registrations'],
        canonicalCookies=cookies, tokenBytes=rows[-1]['keyBytes'],
        sameBytecodeReuseProven=True, sameShapeCacheStorageStable=True,
        uniqueShaderWorkingSetBoundProven=False, includesPipelineOrDriverBytes=False,
        wholeProcessPlateauProven=False, gameMemorySavingsProven=False)


def mutations(rows, windows, token_bytes):
    # A broken original must not make every negative appear "killed".
    check_cache(rows, windows, token_bytes)
    tested = 0
    def reject(value):
        nonlocal tested
        tested += 1
        try: check_cache(value, windows, token_bytes)
        except (ValueError, KeyError, TypeError, IndexError): return
        raise ValueError('shader-cache mutation survived')
    reject([]); reject(rows+rows[-1:]); reject(list(reversed(rows)))
    for index, row in enumerate(rows):
        for field in row:
            value = copy.deepcopy(rows); del value[index][field]; reject(value)
            value = copy.deepcopy(rows)
            if field in (*IDENTITY,*COUNTERS):
                value[index][field] = row[field]+1; reject(value)
                value = copy.deepcopy(rows); value[index][field] = True; reject(value)
            elif field.endswith('Cookies'):
                value[index][field] = [0]*4 if any(row[field]) else [1,0,0,0]; reject(value)
    return dict(tested=tested, killed=tested, source='supplied-cache-observations', gpuRerun=False)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--receipt', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    output = args.output.resolve(); require(not output.exists(), 'analysis output already exists')
    source = args.receipt.resolve(strict=True); baseline = strict_json(source.read_text('utf-8'))
    report = dict(ok=False, baseline=identity(source), gpuRun=False)
    try:
        require(type(baseline['schema']) is int and baseline['schema'] == 1
            and baseline['scope'] == 'M0_ACTUAL_X86_X64_C2_MEMORY_ACCOUNT', 'M0 receipt schema/scope required')
        require(baseline['ok'] is True and baseline['sourceAndReferencePreserved'] is True
            and baseline['playerDllChanged'] is False and baseline['desktopSafety']['ok'] is True,
            'complete passing current M0 baseline required')
        require(len(baseline['cases']) == 1 and baseline['cases'][0]['ok'] is True, 'single M0 workload')
        # Require the complete current M0 source set and original build/copy
        # lineage, not merely whichever identities the caller supplied. The
        # shared reader also replays the actual M0 wire/CPU-account checks.
        verified_baseline, reports, hosts = baseline_reader.verify_baseline(source)
        require(verified_baseline == baseline, 'M0 baseline changed before analysis')
        # These manifests were captured at native launch return. Current bytes
        # cannot be used to retroactively bless an older receipt lacking them.
        initial_raw = memory.verify_raw_receipt(baseline, source.parent)
        case = baseline['cases'][0]
        frozen = baseline['sources']+baseline['binaries']+case['artifacts']
        for row in frozen:
            require(row == identity(Path(row['path'])), 'frozen source/artifact drift')
        stdout = Path(case['process']['stdout']); hostlog = stdout.parent/'host.log'
        require(any(row == identity(stdout) for row in case['artifacts']), 'stdout not frozen by baseline')
        require(any(row == identity(hostlog) for row in case['artifacts']), 'host log not frozen by baseline')
        require(reports == json_lines(stdout) and hosts == json_lines(hostlog),
                'M0 raw records changed before cache analysis')
        actual = check_case(reports, hosts, case['process'], baseline['desktopName'], baseline['desktopSafety']['inputBefore'])
        records = cache_rows(hostlog.read_text('utf-8'))
        singles, _, _, _ = read_records(reports,hosts); summary = singles[SUMMARY]
        token_bytes = summary['vertexShaderBytes']+summary['pixelShaderBytes']
        report['analysis'] = check_cache(records,actual['sampleWindows'],token_bytes)
        report['mutations'] = mutations(records,actual['sampleWindows'],token_bytes)
        report['raw'] = records; report['hostLog'] = identity(hostlog)
        report['memoryAccount'] = actual
        require(all(row == identity(Path(row['path'])) for row in frozen),
                'frozen source/artifact changed during analysis')
        final_baseline, final_reports, final_hosts = baseline_reader.verify_baseline(source)
        require(final_baseline == baseline and final_reports == reports and final_hosts == hosts,
                'M0 baseline or raw records changed during analysis')
        require(memory.verify_raw_receipt(final_baseline, source.parent) == initial_raw,
                'M0 initial raw manifests changed during analysis')
        report['completeCurrentM0SourceSetVerified'] = True
        report['initialRawArtifactsPreserved'] = True
        report['frozenInputsPreserved'] = True
        require(report['baseline'] == identity(source), 'baseline changed during analysis')
        report['ok'] = True
    except Exception as exc:
        report['error'] = str(exc)
    # Preserve any file created by another actor after the initial path check.
    with output.open('x', encoding='utf-8') as stream:
        json.dump(report,stream,indent=2)
    print(json.dumps({k:report.get(k) for k in ('ok','analysis','mutations','error')}))
    return 0 if report['ok'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
