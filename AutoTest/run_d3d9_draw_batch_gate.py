"""Bounded public COM draw batching, real x86 -> x64 DXVK and local A/B.

Offscreen, restricted-Medium non-input Desktop. Timings are queue/CPU/GPU
completion microbenchmarks, never player FPS or full Warcraft migration proof.
"""
from __future__ import annotations
import argparse
import json
import math
import os
from pathlib import Path
import re
import shutil
import statistics
import sys

from run_render_host_protocol_gate import ROOT, identity, pe_identity
from run_render_host_transport_gate import require, strict_json, child_settled
from run_render_host_core_gate import execute, check_host_compile_boundary
from run_render_host_candidate_build import sources, TARGET
from run_render_host_d3d9_gate import json_lines, check_fixture
from run_render_host_presenter_gate import check_launch
from isolated_render_desktop import IsolatedRenderDesktop, validate_probe_receipt
from render_lab_raw_artifacts import capture_raw_artifacts, verify_raw_artifacts
from diagnose_render_host_initialization import ENV_ALLOW
import render_lab_vvl365 as vvl
from run_render_host_validation_gate import check_loader
from render_lab_layer_source import create_empty_source, empty_directory_witness

COMPILERS=Path('E:/Dev/Toolchains/warvk-rh0-llvm-mingw-20260616/portable/llvm-mingw-20260616-ucrt-x86_64/bin')
PROBES=ROOT/'AutoTest/artifacts/host-child-surface-cpu-20261023-r2'
PLAYER=Path('E:/Work/Warcraft III/d3d9.dll')
REFERENCE=ROOT/'build32_device_owner_mdl_low_20261024/src/d3d9/d3d9.dll'

def singleton(rows,schema):
    values=[r for r in rows if r.get('schema')==schema]
    require(len(values)==1,'exactly one '+schema)
    return values[0]

def prefixed(path,prefix):
    return [strict_json(line[len(prefix):]) for line in path.read_text('utf-8').splitlines() if line.startswith(prefix)]

def check_owner_metrics(bs,hs,flights,groups=False,state_reuse=False,stream=False):
    require(bs.get('schema')=='warvk-d3d9-draw-batch-host/v1','batch metrics schema')
    fields=('batches','batchedDraws','largestBatch','constantCapacityBytes','draws','submitted',
            'gpuCompleted','retired','flightCapacity','flightSubmissions','flightReuses',
            'flightRetirements','maxOccupiedFlightSlots','submissionGroupCapacity','groupedSubmissions','nonIndexedDraws',
            'batchStateReuse','redundantStateBindingsSkipped')
    require(set(bs)=={'schema',*fields},'complete batch owner metrics')
    require(all(type(bs[k]) is int and 0<=bs[k]<1<<64 for k in fields),'native unsigned batch counters')
    require(hs.get('ok') is True,'host rejected its own completion')
    require(all(type(hs[k]) is int and hs[k]==bs[k] for k in ('draws','submitted','gpuCompleted','retired')),
            'cold metrics disagree with original owner')
    require(bs['submitted']==bs['gpuCompleted']==bs['retired'],'unretired GPU work')
    require(0<bs['batches']<=bs['draws']<=32*bs['batches'] and 0<bs['largestBatch']<=32
            and bs['batchedDraws']<=bs['draws'],'bounded batch population')
    require(bs['flightCapacity']==(4 if flights else 1),'exact bounded flight mode')
    require(bs['flightSubmissions']==bs['flightRetirements']==(bs['batches'] if flights else 0),
            'all occupied flights retired through original DXVK owner')
    require(0<=bs['maxOccupiedFlightSlots']<=4 and (bs['maxOccupiedFlightSlots']>0)==flights,
            'bounded occupied flight slots')
    require(bs['flightReuses']<=bs['flightSubmissions'],'reuse without a submission')
    # Stream: four native-size (1 MiB) per-binding rings; old storages are
    # renamed through DXVK and bounded by the stream latency check.
    require(bs['constantCapacityBytes']<=(4<<20 if stream else (4 if flights else 1)*262144),
            'constant backing exceeded this tested device/profile bound')
    require(bs['submissionGroupCapacity']==(4 if groups else 1) and (not groups or flights),
            'exact bounded submission grouping')
    require(bs['nonIndexedDraws']<=bs['draws'],'nonindexed population')
    if groups:
        require(0<bs['groupedSubmissions']<=bs['batches']<=4*bs['groupedSubmissions'],
                'every draw batch belongs to one bounded GPU submission')
    else:require(bs['groupedSubmissions']==0,'unexpected grouped submission')
    require(bs['batchStateReuse']==int(state_reuse),'actual batch state mode')
    require(bs['redundantStateBindingsSkipped']<=7*(bs['draws']-bs['batches']),
            'state reuse crossed a batch boundary')
    if state_reuse:require(bs['redundantStateBindingsSkipped']>0,'state optimization did not execute')
    else:require(bs['redundantStateBindingsSkipped']==0,'state optimization unexpectedly enabled')

STREAM_FIELDS=('streamDraws','streamFixedDraws','streamSubmissions','streamCapSubmissions','largestStreamList',
    'maxStreamDraws','maxStreamLatency','constantRenames','latencyWaits','ringBytes','submitted','gpuCompleted','retired')

def check_stream_metrics(rows,bs,stream):
    if not stream:
        require(not rows,'stream metrics without the stream profile');return None
    require(len(rows)==1,'exactly one stream owner metric')
    ss=rows[0]
    require(ss.get('schema')=='warvk-d3d9-stream-host/v1' and set(ss)=={'schema',*STREAM_FIELDS},'complete stream metrics')
    require(all(type(ss[k]) is int and 0<=ss[k]<1<<64 for k in STREAM_FIELDS),'native unsigned stream counters')
    require(all(ss[k]==bs[k] for k in ('submitted','gpuCompleted','retired')),'stream and batch owner disagree')
    require(ss['streamDraws']==bs['draws'] and ss['streamFixedDraws']<=ss['streamDraws'],'every draw recorded by the stream')
    require(ss['maxStreamDraws']==4096 and ss['maxStreamLatency']==3,'exact stream bounds')
    require(0<ss['largestStreamList']<=ss['maxStreamDraws'],'bounded stream list')
    require(0<ss['streamSubmissions']<=ss['submitted'],'stream submissions are real timeline submissions')
    require(ss['streamCapSubmissions']<=ss['streamSubmissions'],'cap flush population')
    require(ss['ringBytes']==bs['constantCapacityBytes'],'ring capacity account')
    return ss

FIXED_BATCH_CAPACITY=20

def fixed_batch_rows(proxy_rows,host_rows,enabled,asynchronous):
    fp=singleton(proxy_rows,'warvk-d3d9-fixed-batch-proxy/v1');fh=singleton(host_rows,'warvk-d3d9-fixed-batch-host/v1')
    require(fp['enabled'] is enabled and fp['asyncAdmission'] is (enabled and asynchronous) and fp['pendingDraws']==0
        and fp['capacity']==fh['capacity']==FIXED_BATCH_CAPACITY,'actual fixed batch admission mode')
    require(fp['admittedDraws']==fh['draws'] and fp['flushedBatches']==fp['remoteSucceededBatches']==fh['batches']
        and 0<=fh['largestBatch']<=FIXED_BATCH_CAPACITY and fh['batches']<=fh['draws']<=FIXED_BATCH_CAPACITY*fh['batches'],
        'every admitted fixed draw and batch reached the host')
    if enabled:require(fh['batches']>0,'fixed batch profile carried no batch')
    else:require(fh['draws']==0,'fixed batches without the fixed batch profile')
    return dict(proxy=fp,host=fh)

def cpu_cost(row,reference):
    fields=('workloadWallTicks','parentKernel100ns','parentUser100ns','hostKernel100ns','hostUser100ns',
            'systemKernel100ns','systemUser100ns','systemIdle100ns','parentCpuCycles','hostCpuCycles')
    require(all(type(row[k]) is int and 0<=row[k]<1<<64 for k in fields),'native unsigned CPU counters')
    require(type(row['qpcFrequency']) is int and 0<row['qpcFrequency']<1<<64 and row['workloadWallTicks']>0,
            'positive workload observation interval')
    require(row['cpuIncludesReadback'] is True and row['hostCpuMeasured'] is (not reference),
            'explicit workload CPU measurement scope')
    require(row['systemIdle100ns']<=row['systemKernel100ns'],'system idle is contained in kernel')
    parent=(row['parentKernel100ns']+row['parentUser100ns'])/10000
    host=(row['hostKernel100ns']+row['hostUser100ns'])/10000
    require(not reference or host==0,'local reference cannot invent host CPU')
    require(row['parentCpuCycles']>0 and ((row['hostCpuCycles']==0) is reference),'real workload cycle counters')
    total=row['systemKernel100ns']+row['systemUser100ns']
    # GetSystemTimes may not advance during a very short valid workload.
    # Preserve this as unavailable, never as an idle/uncontended-machine proof.
    return dict(parentCpuMs=parent,hostCpuMs=host,totalCpuMs=parent+host,
                workloadWallMs=row['workloadWallTicks']*1000/row['qpcFrequency'],
                systemBusyPercent=100*(total-row['systemIdle100ns'])/total if total else None,
                systemCpuAvailable=bool(total),
                systemCpuUnavailableReason=None if total else 'counterDidNotAdvanceInShortWindow',
                parentCpuCycles=row['parentCpuCycles'],hostCpuCycles=row['hostCpuCycles'],
                totalCpuCycles=row['parentCpuCycles']+row['hostCpuCycles'],
                cpuTimeQuantizationCaveat=True,
                includesReadback=True,scope='44_FRAME_WORKLOAD_NOT_PER_FRAME_GPU_TIME')

def check_event_metrics(ps,hs,enabled):
    fields=('eventWaitsPublished','eventWaitsSucceeded','eventPendingObservations','eventLocalCompletions','eventGpuCompleted')
    require(ps['asyncEvents'] is enabled,'actual event completion mode')
    require(all(type(ps[k]) is int and 0<=ps[k]<1<<64 for k in fields),'native unsigned event counters')
    require(all(type(hs[k]) is int and 0<=hs[k]<1<<64 for k in ('queryIssues','queryPolls','queryWaits','gpuCompleted')),
            'native host event counters')
    require(ps['eventWaitsPublished']==ps['eventWaitsSucceeded'],'all event requests settled')
    if enabled:
        require(hs['queryPolls']==0 and 0<hs['queryWaits']==ps['eventWaitsSucceeded']<=hs['queryIssues'],
                'single completion requests replace Poll IPC')
        require(0<ps['eventGpuCompleted']<=hs['gpuCompleted'] and ps['eventLocalCompletions']>0,
                'local observations are covered by actual GPU timeline')
    else:require(all(ps[k]==0 for k in fields),'event mode disabled but observed completion')

def timing(row):
    require(type(row['qpcFrequency']) is int and row['qpcFrequency']>0,'QPC frequency')
    result={}
    for field in ('admissionTicks','completedTicks'):
        values=row[field]
        require(type(values) is list and len(values)==40 and all(type(v) is int and v>0 for v in values),'complete timing samples')
        data=sorted(v*1000/row['qpcFrequency'] for v in values)
        result[field]=dict(medianMs=statistics.median(data),p95Ms=data[math.ceil(.95*len(data))-1],
                           p99Ms=data[math.ceil(.99*len(data))-1],maxMs=max(data))
    require(all(a<=b for a,b in zip(row['admissionTicks'],row['completedTicks'])),'completion precedes admission')
    return result

def check_client_refresh(changed,dependencies):
    # The original build manifest deliberately also listed proxy-only inputs.
    # One rebuilt x86 header may change without changing the immutable x64 EXE.
    # Never infer this solely from its name: verify actual Ninja dependencies,
    # build-system identities and no-work below. All other drift is refused.
    require(len(changed)==1 and Path(changed[0]['path']).resolve()==ROOT/'tools/render_host/d3d9_connection.h',
            'only the explicitly rebuilt x86 Connection header may differ')
    text=dependencies.lower().replace('\\','/')
    require('d3d9_connection.h' not in text,'changed client header is an actual host input')
    require(all(name in text for name in ('d3d9_backend.cpp','d3d9_host_main.cpp','dxvk_context.cpp'))
            and '#deps ' in text and '(valid)' in text,'missing actual host dependency inventory')

def main():
    global REFERENCE
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--reference-dll',type=Path,default=REFERENCE)
    ap.add_argument('--build-receipt',type=Path,required=True)
    ap.add_argument('--output',type=Path,required=True)
    ap.add_argument('--validation',action='store_true',help='pinned VVL365 diagnostic; never performance evidence')
    ap.add_argument('--draws',type=int,choices=(97,1025),default=97)
    ap.add_argument('--wait-profile',action='store_true',help='bounded per-EVENT CPU wait attribution; instrumented cost only')
    ap.add_argument('--event-completion',action='store_true',help='paired async-batch controls versus single GPU event completion')
    ap.add_argument('--gpu-groups',action='store_true',help='paired event controls versus four-packet GPU submission groups')
    ap.add_argument('--state-reuse',action='store_true',help='paired grouped controls versus within-batch redundant state suppression')
    ap.add_argument('--stream',action='store_true',help='paired grouped controls versus one host stream list per sync interval')
    ap.add_argument('--admission-profile',action='store_true',help='bounded per-EVENT host/worker/producer CPU attribution; instrumented cost only')
    ap.add_argument('--fixed-function',action='store_true',help='fixed-function workload: stream DrawFixed controls versus stream fixed batches')
    args=ap.parse_args();out=args.output.resolve()
    REFERENCE=args.reference_dll.resolve()
    require(not args.gpu_groups or args.event_completion,'GPU groups compare the event-completion baseline')
    require(not args.state_reuse or args.gpu_groups,'state comparison requires GPU groups')
    require(not args.stream or (args.gpu_groups and not args.state_reuse),'stream comparison is against GPU groups only')
    require(not args.fixed_function or args.stream,'fixed-function comparison runs on the stream profile')
    require(out.parent==ROOT/'AutoTest/artifacts' and not out.exists(),'fresh output only');out.mkdir()
    receipt=dict(schema='warvk-d3d9-draw-batch-gate/v1',ok=False,scope='OFFSCREEN_REAL_DXVK_BATCH_ABBA',
        gameTouched=False,playerFpsProven=False,fullRendererMigrated=False,steps=[],cases=[],raw=[],copies=[])
    receipt['validationEnabled']=args.validation
    receipt['drawsPerFrame']=args.draws
    receipt['eventCompletionComparison']=args.event_completion
    receipt['gpuGroupingComparison']=args.gpu_groups
    receipt['batchStateComparison']=args.state_reuse
    receipt['streamComparison']=args.stream
    receipt['admissionProfile']=args.admission_profile
    receipt['fixedFunctionWorkload']=args.fixed_function
    require(not (args.admission_profile and args.validation),'admission timing is not a validation run')
    if args.validation:receipt['scope']='OFFSCREEN_REAL_DXVK_BATCH_VVL365_NOT_PERFORMANCE'
    frozen=[]
    env={k:v for k,v in os.environ.items() if k.upper() in ENV_ALLOW}
    env.update(DXVK_LOG_LEVEL='info',DXVK_SHADER_CACHE='0')
    def step(argv,label,timeout=180):
        try:receipt['steps'].append(execute(argv,ROOT,out/label,timeout,env))
        finally:receipt['raw'].append(capture_raw_artifacts(out,[label+'.stdout',label+'.stderr']))
    def copy(source,destination):
        a=identity(source);shutil.copy2(source,destination);b=identity(destination)
        require((a['bytes'],a['sha256'])==(b['bytes'],b['sha256']),'exact copy');receipt['copies'].append(b)
    try:
        build_receipt=strict_json(args.build_receipt.read_text('utf-8'))
        require(build_receipt['ok'] is True and build_receipt['sourcesPreserved'] is True,'green frozen host build')
        host=Path(build_receipt['host']['path']);require(identity(host)==build_receipt['host'],'host build identity')
        require(pe_identity(host)==(0x8664,0x20b),'real x64 host')
        check_host_compile_boundary(Path(build_receipt['build']))
        receipt['buildReceipt']=identity(args.build_receipt.resolve())
        frozen=[identity(p) for p in sources()]
        frozen += [identity(Path(m.__file__).resolve()) for m in list(sys.modules.values())
            if getattr(m,'__file__',None) and Path(m.__file__).suffix=='.py'
            and Path(m.__file__).resolve().is_relative_to(ROOT/'AutoTest')]
        frozen += [identity(p) for p in (Path(__file__).resolve(),ROOT/'AutoTest/test_render_host_d3d9_batch.cpp',
            ROOT/'AutoTest/test_render_host_d3d9_batch_rejection.cpp',ROOT/'AutoTest/test_render_host_d3d9_parent.cpp',
            ROOT/'AutoTest/test_condition_variable_deadline.cpp',
            ROOT/'AutoTest/test_render_host_d3d9_resource_lifetime.cpp',PLAYER,REFERENCE,host,
            PROBES/'desktop-probe-32.exe',PROBES/'desktop-probe-64.exe')]
        receipt['sourcesAndProtected']=frozen.copy()
        if args.validation:
            pinned=ROOT/'AutoTest/artifacts/x64-dispatch-vvl365-candidate-20261023-r1/receipt.json'
            layer=strict_json(pinned.read_text('utf-8'))['layer']
            for kind in ('manifest','library','loader'):
                require(identity(Path(layer[kind]['path']))==layer[kind],'pinned VVL identity '+kind)
                frozen.append(layer[kind])
            frozen.append(identity(pinned))
            manifest=Path(layer['manifest']['path']);library=Path(layer['library']['path']);loader=Path(layer['loader']['path'])
            vvl.check_manifest(strict_json(manifest.read_text('utf-8')))
            receipt['layer']=layer
            receipt['implicitSourceBefore']=create_empty_source(out)
            empty=Path(receipt['implicitSourceBefore']['path'])
        # Build receipt covers every relevant host source, not just the EXE hash.
        changed=[r for r in build_receipt['sources'] if identity(Path(r['path']))!=r]
        if changed:
            build=Path(build_receipt['build'])
            require(identity(build/'build.ninja')==build_receipt['buildNinja']
                    and identity(build/'compile_commands.json')==build_receipt['compileCommands'],
                    'host build system drift')
            step(['ninja','-C',build,'-t','deps'],'host-dependencies',60)
            check_client_refresh(changed,(out/'host-dependencies.stdout').read_text('utf-8'))
            step(['ninja','-C',build,'-n',TARGET.as_posix()],'host-no-work',60)
            require('no work to do' in (out/'host-no-work.stdout').read_text('utf-8'),'host requires rebuild')
            receipt['clientOnlyRefresh']=[dict(before=r,after=identity(Path(r['path']))) for r in changed]
            receipt['hostRebuilt']=False
        cc=COMPILERS/'i686-w64-mingw32-clang++.exe'
        common=[ROOT/'tools/render_host'/p for p in ('protocol.cpp','win32_transport.cpp','win32_security.cpp','owned_child.cpp')]
        flags=['-std=c++17','-O2','-Wall','-Wextra','-Werror','-static','-pthread','-D_WIN32_WINNT=0x0a00']
        proxy=out/'proxy.dll';fixture=out/'batch32.exe';reject=out/'batch-reject32.exe';legacy=out/'legacy32.exe'
        step([cc,*flags,'-Wno-unused-parameter','-Wno-microsoft-exception-spec','-shared','-Wl,--kill-at','-DNOMINMAX',
            ROOT/'tools/render_host/d3d9_proxy.cpp',ROOT/'tools/render_host/d3d9_proxy.def',*common,
            ROOT/'tools/render_host/process_memory_probe.cpp','-luser32','-lbcrypt','-ladvapi32','-luuid','-lpsapi','-o',proxy],'build-proxy')
        for name,source,exe,extra in (
            ('batch','test_render_host_d3d9_batch.cpp',fixture,['-DWARVK_BATCH_TEST_DRAWS='+str(args.draws),
                '-DWARVK_BATCH_TEST_FIXED='+str(int(args.fixed_function))]),
            ('rejection','test_render_host_d3d9_batch_rejection.cpp',reject,[*common,'-lbcrypt','-ladvapi32']),
            ('legacy','test_render_host_d3d9_parent.cpp',legacy,[])):
            step([cc,*flags,'-municode',ROOT/'AutoTest'/source,*extra,'-luser32','-o',exe],'build-'+name)
        deadlines=[]
        for bits,target in ((32,'i686'),(64,'x86_64')):
            exe=out/f'deadline-{bits}.exe'
            step([COMPILERS/(target+'-w64-mingw32-clang++.exe'),*flags,'-DNOMINMAX',
                ROOT/'AutoTest/test_condition_variable_deadline.cpp','-o',exe],f'build-deadline-{bits}')
            require(pe_identity(exe)==((0x14c,0x10b) if bits==32 else (0x8664,0x20b)),'deadline binary width')
            deadlines.append((bits,exe))
        require(all(pe_identity(p)==(0x14c,0x10b) for p in (proxy,fixture,reject,legacy,REFERENCE)),'real x86 callers')
        frozen += [identity(p) for p in (proxy,fixture,reject,legacy)]+[identity(p) for _,p in deadlines]
        with IsolatedRenderDesktop(restricted_medium=True) as desktop:
            receipt['desktopSafety']=desktop.receipt
            probe=desktop.run([PROBES/'desktop-probe-32.exe','--expect-desktop',desktop.desktop_name,
                '--expect-station','WinSta0','--input-desktop',desktop.input_before,
                '--child',PROBES/'desktop-probe-64.exe','--child-log',out/'probe-child.log'],cwd=out,
                stdout_path=out/'probe.stdout',stderr_path=out/'probe.stderr',timeout_s=30,env=env)
            receipt['probe']=probe;receipt['raw'].append(capture_raw_artifacts(out,['probe.stdout','probe.stderr','probe-child.log']))
            check_launch(probe,desktop.desktop_name,desktop.input_before)
            rows=json_lines(out/'probe.stdout')+json_lines(out/'probe-child.log')
            require(len(rows)==2,'both desktop probes')
            validate_probe_receipt(rows[0],station='WinSta0',desktop=desktop.desktop_name,input_desktop=desktop.input_before,
                bits=32,child=rows[1],launch=probe)
            receipt['deadlineTests']=[]
            for bits,exe in deadlines:
                label=f'deadline-{bits}'
                launch=desktop.run([exe],cwd=out,stdout_path=out/(label+'.stdout'),
                    stderr_path=out/(label+'.stderr'),timeout_s=30,env=env)
                receipt['raw'].append(capture_raw_artifacts(out,[label+'.stdout',label+'.stderr']))
                check_launch(launch,desktop.desktop_name,desktop.input_before)
                match=re.fullmatch(r'CONDITION_DEADLINE checks=(\d+) failures=0 bits='+str(bits)+r'\s*',
                    (out/(label+'.stdout')).read_text('utf-8'))
                require(match is not None and int(match[1])>=1014,'exact production deadline tests')
                receipt['deadlineTests'].append(dict(bits=bits,checks=int(match[1]),process=launch))
            cases=[('rejections',reject,False,False),('rejections-flights',reject,False,False),
                   ('legacy-sync',legacy,False,False),('legacy-batch',legacy,True,False),('legacy-flights',legacy,True,False),
                   ('legacy-async-flights',legacy,True,False),
                   ('reference-before',fixture,False,True),('sync-A1',fixture,False,False),('batch-B1',fixture,True,False),
                   ('flight-C1',fixture,True,False),('flight-C2',fixture,True,False),
                   ('async-flight-D1',fixture,True,False),('async-flight-D2',fixture,True,False),
                   ('batch-B2',fixture,True,False),('sync-A2',fixture,False,False),('reference-after',fixture,False,True)]
            if args.event_completion:
                cases=[('rejections-flights',reject,False,False),('legacy-async-event-flights',legacy,True,False),
                       ('reference-before',fixture,False,True),('async-flight-D1',fixture,True,False),
                       ('async-event-flight-E1',fixture,True,False),('async-event-flight-E2',fixture,True,False),
                       ('async-flight-D2',fixture,True,False),('reference-after',fixture,False,True)]
            if args.gpu_groups:
                cases=[('rejections-flights',reject,False,False),('rejections-group-flights',reject,False,False),
                       ('legacy-async-event-group-flights',legacy,True,False),('reference-before',fixture,False,True),
                       ('async-event-flight-E1',fixture,True,False),('async-event-group-flight-G1',fixture,True,False),
                       ('async-event-group-flight-G2',fixture,True,False),('async-event-flight-E2',fixture,True,False),
                       ('reference-after',fixture,False,True)]
            if args.state_reuse:
                cases=[('rejections-group-flights',reject,False,False),('rejections-group-state-flights',reject,False,False),
                       ('legacy-async-event-group-state-flights',legacy,True,False),('reference-before',fixture,False,True),
                       ('async-event-group-flight-S0a',fixture,True,False),('async-event-group-state-flight-S1a',fixture,True,False),
                       ('async-event-group-state-flight-S1b',fixture,True,False),('async-event-group-flight-S0b',fixture,True,False),
                       ('reference-after',fixture,False,True)]
            if args.stream:
                cases=[('rejections-group-flights',reject,False,False),('rejections-stream',reject,False,False),
                       ('legacy-async-event-stream',legacy,True,False),('reference-before',fixture,False,True),
                       ('async-event-group-flight-G1',fixture,True,False),('async-event-stream-T1',fixture,True,False),
                       ('async-event-stream-T2',fixture,True,False),('async-event-group-flight-G2',fixture,True,False),
                       ('reference-after',fixture,False,True)]
            if args.fixed_function:
                cases=[('rejections-stream',reject,False,False),('legacy-async-event-stream',legacy,True,False),
                       ('reference-before',fixture,False,True),('async-event-stream-T1',fixture,True,False),
                       ('async-event-stream-fixed-F1',fixture,True,False),('async-event-stream-fixed-F2',fixture,True,False),
                       ('async-event-stream-T2',fixture,True,False),('reference-after',fixture,False,True)]
            if args.validation:
                selected=('rejections-flights','legacy-async-event-flights','async-event-flight-E1') if args.event_completion else (
                    'rejections-flights','legacy-async-flights','async-flight-D1')
                if args.gpu_groups:selected=('rejections-group-flights','legacy-async-event-group-flights','async-event-group-flight-G1')
                if args.state_reuse:selected=('rejections-group-state-flights','legacy-async-event-group-state-flights','async-event-group-state-flight-S1a')
                if args.stream:selected=('rejections-stream','legacy-async-event-stream','async-event-stream-T1')
                if args.fixed_function:selected=('rejections-stream','legacy-async-event-stream','async-event-stream-fixed-F1')
                cases=[c for c in cases if c[0] in selected]
            for label,program,batching,reference in cases:
                folder=out/label;folder.mkdir();local_exe=folder/'fixture.exe';local_dll=folder/'renderer.dll'
                copy(program,local_exe);copy(REFERENCE if reference else proxy,local_dll)
                local=env|dict(DXVK_LOG_PATH=str(folder),DXVK_CONFIG_FILE=str(folder/'absent.conf'))
                if not reference:
                    local_host=folder/'host.exe';copy(host,local_host)
                    local.update(WARVK_D3D9_HOST_EXE=str(local_host),WARVK_D3D9_HOST_LOG=str(folder/'host.log'))
                if batching:local['WARVK_D3D9_BATCH_DRAWS']='1'
                flights='flight' in label
                if flights:local['WARVK_D3D9_GPU_FLIGHTS']='4'
                if args.wait_profile and flights:local['WARVK_D3D9_WAIT_PROFILE']='1'
                groups='group' in label
                if groups:local['WARVK_D3D9_GPU_GROUP']='4'
                state_reuse='state' in label
                if state_reuse:local['WARVK_D3D9_BATCH_STATE']='1'
                stream='stream' in label
                if stream:local['WARVK_D3D9_STREAM']='1'
                fixed_batch='fixed' in label
                if fixed_batch:local['WARVK_D3D9_FIXED_BATCH']='1'
                # Only batch arms have a per-batch admission population.
                admission=args.admission_profile and program==fixture and not reference and (fixed_batch or not args.fixed_function)
                if admission:local['WARVK_D3D9_ADMISSION_PROFILE']='1'
                asynchronous='async' in label
                if asynchronous:local['WARVK_D3D9_ASYNC_BATCH']='1'
                events='event' in label
                if events:local['WARVK_D3D9_ASYNC_EVENTS']='1'
                if args.validation:local.update(vvl.validation_env(folder,manifest,empty))
                command=[local_exe,'--dll',local_dll,'--scenario','normal']
                if reference:command+=['--reference']
                if program==legacy:command+=['--present']
                if program==reject:command=[local_exe,'--mode','raw','--dll',local_dll,
                    '--expect-desktop',desktop.desktop_name,'--input-desktop',desktop.input_before]
                case=dict(label=label,ok=False);receipt['cases'].append(case)
                case['process']=desktop.run(command,cwd=folder,stdout_path=folder/'case.stdout',stderr_path=folder/'case.stderr',
                    timeout_s=90,env=local)
                raw=capture_raw_artifacts(folder,['case.stdout','case.stderr']+([] if reference else ['host.log'])
                    +(['validation.raw.log'] if args.validation else []),True)
                receipt['raw'].append(raw)
                check_launch(case['process'],desktop.desktop_name,desktop.input_before)
                reports=json_lines(folder/'case.stdout');case['reports']=reports
                if not reference:
                    pr=singleton(reports,'warvk-d3d9-proxy/v1');hs=singleton(json_lines(folder/'host.log'),'warvk-d3d9-host/v1')
                    ds=singleton(json_lines(folder/'host.log'),'warvk-d3d9-desktop/v1')
                    case['host']=hs
                    require(pr['peerVerified'] and pr['x86VulkanAbsent'] and pr['childNaturalExit'] and pr['childProcessSettled']
                        and not pr['transportFailed'] and not pr['deviceLost'] and pr['childExit']==0,'real healthy remote teardown')
                    require(hs['bits']==64 and hs['actualDxvkCore'] and hs['actualDxso'] and hs['normalTeardown']
                        and hs['shutdownQueueDrained'] and hs['liveResources']==0,'actual x64 owner drained')
                    require(ds['pid']==pr['childPid'] and ds['peerPid']==case['process']['pid']
                        and ds['desktop']==desktop.desktop_name and ds['inputDesktop']==desktop.input_before,'bound host identity/desktop')
                    require(hs['submitted']==hs['gpuCompleted']==hs['retired'],'GPU and DXVK retirement')
                    require(pr['exchanges']==hs['accepted']+hs['rejected'],'command balance')
                    case['childSettlement']=child_settled(pr['childPid'],pr['childCreation'])
                    batches=prefixed(folder/'host.log','D3D9 draw-batch-host: ')
                    require(len(batches)==1,'batch owner metrics');case['batchHost']=batches[0]
                    bs=case['batchHost']
                    check_owner_metrics(bs,hs,flights,groups,state_reuse,stream)
                    case['streamHost']=check_stream_metrics(prefixed(folder/'host.log','D3D9 stream-host: '),bs,stream)
                    ff=args.fixed_function and program==fixture
                    ps=prefixed(folder/'case.stderr','D3D9 draw-batch-proxy: ')
                    require(len(ps)==1 and ps[0]['asyncAdmission'] is asynchronous
                            and ps[0]['enabled'] is batching,'actual draw admission mode')
                    require(ps[0]['pendingDraws']==0 and ps[0]['remoteSucceededBatches']==(0 if ff else bs['batches'] if batching or program==reject else 0),
                            'pending or unacknowledged batches at shutdown')
                    if ff:require(ps[0]['admittedDraws']==ps[0]['flushedBatches']==0,'fixed workload staged a programmable draw')
                    elif batching:require(ps[0]['admittedDraws']==hs['draws'] and ps[0]['flushedBatches']==bs['batches'],
                            'each admitted draw and batch reached host')
                    case['fixedBatch']=fixed_batch_rows(prefixed(folder/'case.stderr','D3D9 fixed-batch-proxy: '),
                        prefixed(folder/'host.log','D3D9 fixed-batch-host: '),fixed_batch,asynchronous)
                    fh=case['fixedBatch']['host']
                    if ff and fixed_batch:require(fh['draws']==hs['draws'] and fh['batches']==bs['batches'],'every fixed draw was batched')
                    elif ff:require(bs['batches']==hs['draws'],'each unbatched fixed draw is one synchronous DrawFixed')
                    case['batchProxy']=ps[0]
                    check_event_metrics(ps[0],hs,events)
                if program==reject:
                    row=singleton(reports,'warvk-d3d9-batch-rejection/v1')
                    require(row['ok'] and row['rejected']==row['recoveries']==29 and row['noPrefixPixelChecks']==29*4096
                        and row['pixelFailures']==0,'real atomic rejection and recovery')
                    require(row['destroyPendingBatches']==5 and row['reusedResourceTypes']==6
                        and row['staleAfterReuseRejected']==6 and row['recoveryPixelChecks']==48*3328,
                        'destroy in-flight, reuse all typed IDs, reject stale keys, recover new image')
                    require(row['eventWaitRejected']==12 and row['eventWaitRecovered']==3,
                            'wait event identity, revision, destruction and reuse')
                    deferred=int(groups or stream)
                    require(row['nonIndexedRejected']==12 and row['pendingDiagnosticRefused']==deferred,
                            'nonindexed atomic validation and unsubmitted GPU group visibility')
                    require(hs['draws']==35*32 and hs['rejected']==35+12+12+deferred,'no rejected draw prefix')
                elif program==legacy:
                    check_fixture(singleton(reports,'warvk-d3d9-api-fixture/v1'),'normal')
                    require(hs['draws']==3,'legacy draw coverage')
                else:
                    row=singleton(reports,'warvk-d3d9-batch-fixture/v1')
                    require(row['ok'] and row['normalTeardown'] and row['bits']==32 and row['reference'] is reference
                        and row['fixedFunction'] is args.fixed_function
                        and row['frames']==44 and row['warmFrames']==4 and row['drawsPerFrame']==args.draws
                        and row['pixelChecks']==44*32*9 and row['pixelFailures']==0,'complete workload')
                    require(row['coverageDraws']==193 and row['coverageChecks']==576,'visible earlier-batch/upload coverage')
                    require(row['nonIndexedDraws']==32,'actual nonindexed coverage population')
                    require(row['stateTransitionDraws']==64,'visible adjacent depth/blend/write-mask transitions')
                    require(row['queryTransitions']==5,'repeat Issue, independent Query and destroy/reuse')
                    staged=fixed_batch if args.fixed_function else batching
                    require(row['diagnosticPendingRefusals']==int(staged) and row['diagnosticAfterFlush']==int(not reference),
                            'read-only diagnostics cannot implicitly flush staged draws')
                    require(row['gameTouched'] is False and row['playerFpsProven'] is False,'evidence bounds')
                    case['pixelHash']=row['imageHash']
                    case['coverageHash']=row['coverageHash']
                    if not args.validation:
                        case['timing']=timing(row)
                        case['cpuCost']=cpu_cost(row,reference)
                    if args.wait_profile and flights:
                        from analyze_d3d9_reuse_wait import analyze
                        case['reuseWait']=analyze(prefixed(folder/'host.log','D3D9 reuse-wait: '), row, groups=groups)
                    if not reference:
                        rows_by_owner=(prefixed(folder/'host.log','D3D9 admission-host: '),
                                       prefixed(folder/'case.stderr','D3D9 admission-worker: '),
                                       prefixed(folder/'case.stderr','D3D9 admission-producer: '))
                        if admission:
                            from analyze_d3d9_admission import analyze as analyze_admission
                            case['admission']=analyze_admission(*rows_by_owner, row, capacity=20 if args.fixed_function else 32)
                        else:require(not any(rows_by_owner),'admission samples without the admission profile')
                    if not reference:
                        require(row['parentVulkanAbsent'] is True and hs['draws']==44*args.draws+193,'render workload outside x86')
                        capacity=(20 if fixed_batch else 1) if args.fixed_function else (32 if batching else 1)
                        perFrame=math.ceil(args.draws/capacity)
                        # Coverage: 129 draws, then 32 | texture upload | 32 draws.
                        coverage={32:5+2,20:7+4,1:193}[capacity]
                        require(case['batchHost']['batches']==44*perFrame+coverage,'bounded batch submissions')
                        require(case['batchHost']['largestBatch']==capacity,'batch capacity')
                        require(bs['nonIndexedDraws']==32,'host executed all nonindexed draws')
                        if groups:
                            require(bs['maxOccupiedFlightSlots']==4 and bs['flightReuses']>0
                                    and bs['groupedSubmissions']==44*math.ceil(perFrame/4)+4,
                                    'bounded grouping plus EndScene/upload flush boundaries')
                        elif flights:
                            require(bs['maxOccupiedFlightSlots']==4 and bs['flightReuses']==44*max(0,perFrame-4)+1,
                                'exact bounded flight reuse population')
                        elif stream:
                            ss=case['streamHost']
                            # EndScene FlushDraws closes each measured frame's single list.
                            require(ss['streamSubmissions']>=44 and ss['streamCapSubmissions']==0
                                    and ss['largestStreamList']>=args.draws and ss['constantRenames']>0
                                    and ss['streamFixedDraws']==(hs['draws'] if args.fixed_function else 0),'one stream list per measured frame')
                if args.validation:
                    case['validation']=vvl.parse_validation((folder/'validation.raw.log').read_text('utf-8'))
                    case['loader']=check_loader((folder/'host.log').read_text('utf-8'),
                        (folder/'host_warvk-d3d9-host.log').read_text('utf-8'),library,empty,loader)
                case['ok']=True
            samples=[c for c in receipt['cases'] if 'timing' in c]
            if args.validation:
                require(len(receipt['cases'])==3 and not samples,'VVL population is not a timing comparison')
                receipt['implicitSourceAfter']=empty_directory_witness(empty)
                require(receipt['implicitSourceBefore']==receipt['implicitSourceAfter'],'private layer source drift')
            else:
                require(len(samples)==(6 if args.event_completion else 10) and len({c['pixelHash'] for c in samples})==1,'reference/remote identical sampled pixels')
                require(len({c['coverageHash'] for c in samples})==1,'reference/remote batch lifetime and texture upload pixels')
                receipt['matchedSelectedPixels']=True
            receipt['timingsArePlayerFps']=False
        require(receipt['desktopSafety']['ok'],'isolated cleanup')
        receipt['ok']=True
    except Exception as error:receipt['error']=str(error)
    finally:
        try:
            receipt['inputsPreserved']=all(identity(Path(r['path']))==r for r in frozen)
            require(bool(frozen) and receipt['inputsPreserved'],'input drift')
            require(all(identity(Path(r['path']))==r for r in receipt['copies']),'case copy drift')
            for raw in receipt['raw']:verify_raw_artifacts(raw)
            receipt['rawPreserved']=True
        except Exception as error:receipt['ok']=False;receipt['preservationError']=str(error)
        (out/'receipt.json').write_text(json.dumps(receipt,indent=2),encoding='utf-8')
    print(json.dumps({k:receipt.get(k) for k in ('ok','error','preservationError','inputsPreserved','matchedSelectedPixels')}))
    return 0 if receipt['ok'] else 1

if __name__=='__main__':raise SystemExit(main())
