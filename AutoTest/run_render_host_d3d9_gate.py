"""Actual x86 public D3D9 calls -> x64 existing DXSO/DXVK, offscreen only.

Synchronous development COM profile, not a shipping renderer. No game/WSI/
release/memory-saving claim. Every child is held by exact-process Job ownership.
The preserved single-process DLL is loaded only by a separate hidden fixture.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

from run_render_host_protocol_gate import ROOT, identity, pe_identity
from run_render_host_transport_gate import require, strict_json, child_settled
from run_render_host_core_gate import execute, source_paths, check_host_compile_boundary


def native_uint(value, bits=64):
    return type(value) is int and 0 <= value < 1 << bits


def check_render_observation(f, scenario, reference=False):
    """Rendering evidence only. Does NOT accept normal teardown or a full run."""
    require(type(f) is dict and type(reference) is bool and
            type(scenario) is str and scenario in ('normal', 'invalid'), 'checker scope')
    require(f['schema'] == 'warvk-d3d9-api-fixture/v1' and
            f['bits'] == 32 and type(f['bits']) is int, 'real x86 API fixture')
    require(f['reference'] is reference and f['scenario'] == scenario, 'fixture scope')
    require(type(f['parentVulkanAbsent']) is bool, 'process diagnostic type')
    for flag in ('gameTouched', 'globalInputUsed', 'productIntegrated', 'memorySavingsProven'):
        require(f[flag] is False, f'unsupported claim {flag}')
    for flag in ('publicD3D9Api', 'shaderBytesRoundtripped', 'queryCompleted', 'readbackVerified'):
        require(f[flag] is True, f'incomplete API gate {flag}')
    exact = dict(width=64, height=64, drawCalls=3, primitiveCount=6, vertexBufferBytes=131101,
                 streamOffset=65560, vertexStride=40, fullVertexUploadBytes=131101,
                 partialVertexUploadBytes=160, fullIndexUploadBytes=12, partialIndexUploadBytes=2,
                 textureUploadBytes=16)
    for key, value in exact.items():
        require(native_uint(f[key]) and f[key] == value, f'API population: {key}')
    for key in ('queryPolls', 'queryPending', 'vertexShaderBytes', 'pixelShaderBytes', 'comIdentityChecks',
                'invalidChecks', 'profileRestrictionChecks', 'processBoundaryChecks', 'presentHresult',
                'badVertexShaderBytes', 'backendRejectionChecks', 'idleRequestedMs'):
        require(native_uint(f[key], 32), f'fixture integer: {key}')
    invalid = scenario == 'invalid'
    require(f['queryPolls'] == f['queryPending'] + 1 + int(invalid and not reference), 'actual EVENT completion')
    for key in ('vertexShaderBytes', 'pixelShaderBytes'):
        require(16 < f[key] <= 4096*4 and f[key] % 4 == 0, 'original bounded D3D9 tokens')
    require(f['comIdentityChecks'] >= 8 and f['processBoundaryChecks'] >= 3, 'COM/process coverage')
    require(f['invalidChecks'] == ((3 if reference else 7) if invalid else 0), 'negative API coverage')
    require(f['profileRestrictionChecks'] == (2 if invalid and not reference else 0), 'restricted profile negatives')
    recovery = invalid and not reference
    require(f['backendRejectionChecks'] == int(recovery), 'actual backend rejection coverage')
    require(native_uint(f['idleElapsedMs']), 'idle wait witness type')
    if recovery:
        require(16 < f['badVertexShaderBytes'] <= 4096*4 and f['badVertexShaderBytes'] % 4 == 0,
                'independent declaration-mismatch shader')
        require(f['idleRequestedMs'] == 6500 and 6500 <= f['idleElapsedMs'] < 120000,
                'actual healthy idle longer than framing deadline')
    else:
        require(f['badVertexShaderBytes'] == f['idleRequestedMs'] == f['idleElapsedMs'] == 0,
                'unexpected recovery workload')
    require(f['queryValueGuardChecked'] is (invalid and not reference), 'BOOL4 query guard')
    p = f['pixelOracle']
    require(type(p) is dict and type(p['firstFailure']) is dict, 'pixel diagnostics shape')
    for key, value in dict(checked=2548, clear=1392, opaque=960, blend=196, depthProtected=416, failures=0).items():
        require(native_uint(p[key], 32) and p[key] == value, f'independent pixel population: {key}')
    require(native_uint(p['maxChannelError'], 32) and p['maxChannelError'] <= 1, 'pixel error')
    for key in ('x', 'y', 'actualBGRA', 'expectedBGRA'):
        require(native_uint(p['firstFailure'][key], 32) and p['firstFailure'][key] == 0, 'unexpected failed pixel')
    for value in (p['imageFnv'], f['vertexShaderFnv'], f['pixelShaderFnv']):
        require(type(value) is str and re.fullmatch('[0-9a-f]{16}', value), 'full 64-bit hashes')
    if not reference:
        require(f['parentVulkanAbsent'] is True, 'x86 must not load Vulkan')
        require(f['present'] == 'explicitly-unsupported' and native_uint(f['presentHresult'], 32)
                and f['presentHresult'] == 0x80004001, 'offscreen must not claim Present')
    else:
        require(f['present'] == 'not-requested' and f['presentHresult'] == 0x80004001, 'offscreen reference scope')


def check_fixture(f, scenario, reference=False):
    check_render_observation(f, scenario, reference)
    require(f['ok'] is True and f['stage'] == 'complete' and f['error'] == '', 'fixture did not fully complete')
    for flag in ('resourceHeldDevice', 'normalTeardown', 'windowClosed'):
        require(f[flag] is True, f'incomplete lifetime gate {flag}')
    for key in ('deviceRefsWhileResourceHeld', 'windowMessages'):
        require(native_uint(f[key], 32) and f[key] > 0, f'lifetime witness {key}')
    for key in ('deviceFinalRefs', 'factoryFinalRefs'):
        require(native_uint(f[key], 32) and f[key] == 0, f'final external references {key}')


def check_remote(scenario, fixture, proxy, host):
    check_fixture(fixture, scenario)
    require(type(proxy) is dict and type(host) is dict, 'peer report shape')
    require(proxy['schema'] == 'warvk-d3d9-proxy/v1' and host['schema'] == 'warvk-d3d9-host/v1', 'peer schemas')
    require(native_uint(proxy['childPid'], 32) and proxy['childPid'] > 0 and
            native_uint(proxy['childCreation']) and proxy['childCreation'] > 0, 'exact child identity')
    require(proxy['childExit'] == 0 and type(proxy['childExit']) is int and proxy['peerVerified'] is True
            and proxy['transportFailed'] is False and proxy['x86VulkanAbsent'] is True, 'exact transport settlement')
    require(host['ok'] is True and host['bits'] == 64 and type(host['bits']) is int and
            host['actualDxvkCore'] is True and host['actualDxso'] is True, 'real x64 existing compiler/renderer')
    for result in (host, proxy):
        for field in ('productIntegrated', 'memorySavingsProven'):
            require(result[field] is False, f'unsupported claim {field}')
    require(host['gameTouched'] is False and host['presentImplemented'] is False and host['normalTeardown'] is True,
            'offscreen scope/actual normal teardown')
    extra = 1 if scenario == 'invalid' else 0
    # init + texture init(s) + 7 uploads + clear + 3 draws + readback.
    submissions = 14 + extra
    peak = (131101 + 12 + 16*(1+extra) + 24 + fixture['vertexShaderBytes']
            + fixture['pixelShaderBytes'] + fixture['badVertexShaderBytes'])
    # C2 Query owns a logical handle: one CPU-only Create and Destroy, not
    # additional GPU work. The underlying Issue/Poll population is unchanged.
    accepted = 29 + fixture['queryPolls'] + extra*4
    expected = dict(accepted=accepted, rejected=1+extra,
                    backendCalls=accepted-2+extra,
                    submitted=submissions, gpuCompleted=submissions, retired=submissions,
                    draws=3, clears=1, uploads=7, uploadedBytes=131291, readbacks=1, queries=1,
                    buffersCreated=2, texturesCreated=1+extra, shadersCompiled=2+extra, declarationsCreated=1,
                    liveResources=0, liveLogicalBytes=0, peakLogicalBytes=peak,
                    liveLogicalHandles=0, liveQueries=0, queryHandlesCreated=1, queryHandlesDestroyed=1,
                    queryIssues=1, queryPolls=fixture['queryPolls'])
    for key, value in expected.items():
        require(native_uint(host[key]) and host[key] == value, f'host exact population {key}')
    require(native_uint(proxy['exchanges']) and proxy['exchanges'] == host['accepted']+host['rejected'], 'command balance')
    require(native_uint(host['vendorId'], 32) and host['vendorId'] > 0 and
            native_uint(host['deviceId'], 32) and host['deviceId'] > 0, 'GPU IDs')


def json_lines(path):
    return [strict_json(s) for s in path.read_text('utf-8').splitlines() if s.startswith('{')]


def check_idle_cpu(records, bits):
    require(type(records) is list and len(records) == 4 and type(bits) is int and bits in (32,64),
            'three idle cases and one CPU summary required')
    summary = records[-1]
    expected = dict(schema='warvk-idle-transport/v1', cases=3, passed=3, childProcesses=3,
                    cpuOnly=True, gameTouched=False, externalWatchdogRequired=True)
    require(type(summary) is dict and summary.keys()==expected.keys() and
            all(type(summary[k]) is type(v) and summary[k]==v for k,v in expected.items()), 'idle transport completion')
    identities = []
    for row, name in zip(records[:-1], ('idle6500', 'partial-header', 'peer-exit')):
        require(type(row) is dict and row['case'] == name and row['ok'] is True and row['cpuOnly'] is True and
                native_uint(row['bits'], 32) and row['bits'] == bits and row['peerVerified'] is True, 'idle case scope')
        for key in ('childPid', 'childCreation', 'childExit', 'elapsedMs', 'framingDeadlineMs',
                    'reads', 'cancelRequests', 'cancelCompletions'):
            require(native_uint(row[key]), 'idle numeric type: '+key)
        require(0 < row['childPid'] < 1 << 32 and row['childCreation'] > 0 and row['childExit'] == 0,
                'idle exact child identity/exit')
        require(row['framingDeadlineMs'] == 5000 and 0 <= row['elapsedMs'] < 30000, 'idle deadline unchanged')
        cancelled = int(name != 'idle6500')
        require(row['cancelRequests'] == row['cancelCompletions'] == cancelled, 'idle actual cancellation drain')
        require(row['reads'] >= (1 if name == 'peer-exit' else 2), 'real kernel reads')
        if name == 'idle6500':require(row['elapsedMs'] >= 6000, 'healthy idle was not exercised')
        if name == 'partial-header':require(row['elapsedMs'] >= 4950, 'partial header failed for wrong reason')
        identities.append((row['childPid'],row['childCreation']))
    require(len(set(identities))==3,'three independently owned child lifetimes required')
    return identities


def observe_case(argv, cwd, prefix, timeout, env):
    """Retain a failed fixture's report; a nonzero exit never becomes a pass."""
    started = time.monotonic()
    flags = subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS
    with prefix.with_suffix('.stdout').open('xb') as out, prefix.with_suffix('.stderr').open('xb') as err:
        process = subprocess.Popen(list(map(str, argv)), cwd=cwd, env=env, stdout=out, stderr=err, creationflags=flags)
        try:
            code = process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            process.kill()  # Exact owned standalone fixture; never a game.
            process.wait(timeout=10)
            raise RuntimeError(f'{prefix}: whole-process watchdog failure')
    return dict(command=list(map(str, argv)), exit=code, seconds=time.monotonic()-started)


def sources():
    paths = set(source_paths())
    for domain in ('src/dxso', 'tools/render_host'):
        for p in (ROOT/domain).rglob('*'):
            if p.is_file() and (p.suffix in ('.h', '.cpp', '.def') or p.name == 'meson.build'):
                paths.add(p)
    # Includes shared options/FF SPIR-V definitions and actual shipping caller;
    # broad header coverage does not mean the x64 target compiles game hooks.
    paths.update((ROOT/'src/d3d9').rglob('*.h'))
    paths.update(ROOT/p for p in ('src/d3d9/d3d9_device.cpp', 'src/d3d9/d3d9_options.cpp',
        'src/d3d9/d3d9_fixed_function.cpp', 'src/d3d9/d3d9_shader_helpers.cpp', 'src/d3d9/d3d9_shader.cpp',
        'AutoTest/test_render_host_d3d9_parent.cpp', 'AutoTest/test_render_host_d3d9_shader_tokens.cpp',
        'AutoTest/test_render_host_d3d9_wire.cpp', 'AutoTest/test_render_host_d3d9_gate.py',
        'AutoTest/test_render_host_d3d9_resource_registry.cpp',
        'AutoTest/test_render_host_idle_transport.cpp', 'AutoTest/run_render_host_d3d9_gate.py'))
    return sorted(paths)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--toolchain', type=Path, required=True)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--reference-dll', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=False, exist_ok=False)
    receipt = dict(schema=1, scope='ACTUAL_PUBLIC_D3D9_X86_TO_EXISTING_DXVK_X64_OFFSCREEN_SUBSET', ok=False,
                   gameLaunched=False, shippingDllBuilt=False, playerDllChanged=False, productIntegrated=False,
                   presenterImplemented=False, gameMemorySavingsProven=False, gpuValidationLayerTested=False,
                   synchronousDevelopmentOwner=True, watchdogTerminationIsFailure=True, steps=[], cases=[], cpu=[])
    try:
        build = args.build.resolve(strict=True)
        ref = args.reference_dll.resolve(strict=True)
        options = {o['name']: o['value'] for o in strict_json((build/'meson-info/intro-buildoptions.json').read_text('utf-8'))}
        require(options['enable_d3d9'] is False and options['warvk_render_host_core_dev'] is True, 'isolated x64 build only')
        receipt['allowedSharedD3D9Inputs'] = check_host_compile_boundary(build)
        require(len(receipt['allowedSharedD3D9Inputs']) == 2, 'shared options and shader helpers required')
        inputs = sources();before = [identity(p) for p in inputs];receipt['sources'] = before
        receipt['referenceDll'] = identity(ref)
        require(pe_identity(ref) == (0x14c, 0x10b), 'reference x86 PE')
        cc = {bits: args.toolchain.resolve(strict=True)/(arch+'-w64-mingw32-clang++.exe')
              for bits, arch in ((32, 'i686'), (64, 'x86_64'))}
        receipt['compilers'] = [identity(p) for p in cc.values()]
        receipt['steps'].append(execute([sys.executable, '-B', ROOT/'AutoTest/test_render_host_d3d9_gate.py'],
                                       ROOT, out/'receipt-checker', 30))
        for bits in (32, 64):
            for test in ('shader_tokens', 'wire'):
                exe = out/f'{test}-{bits}.exe'
                src = [ROOT/f'AutoTest/test_render_host_d3d9_{test}.cpp']
                if test == 'wire': src += [ROOT/'tools/render_host/protocol.cpp']
                receipt['steps'].append(execute([cc[bits], '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-static', *src, '-o', exe], ROOT, out/f'compile-{test}-{bits}', 180))
                require(pe_identity(exe) == ((0x14c,0x10b) if bits==32 else (0x8664,0x20b)), 'CPU executable width')
                receipt['steps'].append(execute([exe], out, out/f'test-{test}-{bits}', 30))
                result=(out/f'test-{test}-{bits}.stdout').read_text('utf-8').strip()
                require(re.search(rf'checks=\d+ failures=0 bits={bits}\b',result) is not None, 'CPU test completion')
                receipt['cpu'].append(dict(test=test,bits=bits,result=result,exe=identity(exe)))
            idle=out/f'idle-transport-{bits}.exe'
            idle_dir=out/f'idle-{bits}';idle_dir.mkdir()
            common=[ROOT/'tools/render_host'/p for p in
                    ('protocol.cpp','win32_transport.cpp','win32_security.cpp','owned_child.cpp')]
            receipt['steps'].append(execute([cc[bits],'-std=c++17','-O2','-Wall','-Wextra','-Werror','-static',
                '-municode',ROOT/'AutoTest/test_render_host_idle_transport.cpp',*common,'-lbcrypt','-ladvapi32','-o',idle],
                ROOT,out/f'compile-idle-{bits}',180))
            require(pe_identity(idle)==((0x14c,0x10b) if bits==32 else (0x8664,0x20b)), 'idle fixture width')
            receipt['steps'].append(execute([idle,'--artifacts',idle_dir],out,out/f'test-idle-{bits}',45))
            records=json_lines(out/f'test-idle-{bits}.stdout')
            settlements=[child_settled(pid,creation) for pid,creation in check_idle_cpu(records,bits)]
            receipt['cpu'].append(dict(test='idle_transport',bits=bits,results=records,settlements=settlements,exe=identity(idle)))
        target='tools/render_host/dxvk_core/warvk-d3d9-host-dev.exe'
        receipt['steps'].append(execute(['ninja','-C',build,'-j2',target],ROOT,out/'host-build',600))
        receipt['steps'].append(execute(['ninja','-C',build,'-n',target],ROOT,out/'host-no-work',30))
        require('no work to do' in (out/'host-no-work.stdout').read_text('utf-8'), 'host no-work')
        require(check_host_compile_boundary(build)==receipt['allowedSharedD3D9Inputs'], 'build boundary changed at regeneration')
        receipt['compileCommands']=identity(build/'compile_commands.json')
        host=build/target;require(pe_identity(host)==(0x8664,0x20b),'x64 renderer')
        receipt['hostExe']=identity(host)
        proxy=out/'warvk-d3d9-proxy-dev.dll'
        common=[ROOT/'tools/render_host'/p for p in ('d3d9_proxy.cpp','d3d9_proxy.def','protocol.cpp',
                'win32_transport.cpp','win32_security.cpp','owned_child.cpp','process_memory_probe.cpp')]
        receipt['steps'].append(execute([cc[32],'-std=c++17','-O2','-Wall','-Wextra','-Werror',
            '-Wno-unused-parameter','-Wno-microsoft-exception-spec','-static','-pthread','-shared',
            '-Wl,--kill-at','-DNOMINMAX','-D_WIN32_WINNT=0x0a00',*common,'-lbcrypt','-ladvapi32','-luuid','-lpsapi','-o',proxy],
            ROOT,out/'proxy-build',240))
        parent=out/'d3d9-api-parent32.exe'
        receipt['steps'].append(execute([cc[32],'-std=c++17','-O2','-Wall','-Wextra','-Werror','-static',
            '-municode',ROOT/'AutoTest/test_render_host_d3d9_parent.cpp','-luser32','-o',parent],ROOT,out/'parent-build',180))
        require(pe_identity(proxy)==pe_identity(parent)==(0x14c,0x10b),'x86 public API binaries')
        receipt['proxyDll']=identity(proxy);receipt['parentExe']=identity(parent)
        env={k:v for k,v in os.environ.items() if not k.upper().startswith(('DXVK_','VK_','WARVK_D3D9_'))}
        env.update(DXVK_LOG_LEVEL='info',DXVK_SHADER_CACHE='0')
        for reference in (False, True):
            for scenario in ('normal','invalid'):
                label=('reference-' if reference else 'remote-')+scenario
                directory=out/label;directory.mkdir()
                local=env|dict(DXVK_LOG_PATH=str(directory),DXVK_CONFIG_FILE=str(directory/'absent.conf'))
                cmd=[parent,'--dll',ref if reference else proxy,'--scenario',scenario]
                if reference:cmd+=['--reference']
                else:
                    local.update(WARVK_D3D9_HOST_EXE=str(host),WARVK_D3D9_HOST_LOG=str(directory/'host.log'))
                    cmd+=['--present']
                case=dict(label=label,ok=False,renderObservationVerified=False)
                try:
                    step=observe_case(cmd,directory,directory/'parent',120,local)
                    receipt['steps'].append(step);case['exit']=step['exit']
                    records=json_lines(directory/'parent.stdout')
                    fixtures=[r for r in records if r.get('schema')=='warvk-d3d9-api-fixture/v1']
                    require(len(fixtures)==1,'one fixture report');f=fixtures[0];case['fixture']=f
                    # An image can be observed before a later lifetime failure.
                    # Keep this separate; do not patch ok/error/refcounts to
                    # force a failed preserved reference through check_fixture.
                    check_render_observation(f,scenario,reference)
                    require(f['stage'] in ('complete','bound-resource-com-teardown'), 'render stage incomplete')
                    case['renderObservationVerified']=True
                    require(step['exit']==0,'fixture process failed: '+str(step['exit']))
                    if reference:check_fixture(f,scenario,True)
                    else:
                        proxies=[r for r in records if r.get('schema')=='warvk-d3d9-proxy/v1']
                        hosts=json_lines(directory/'host.log');require(len(proxies)==len(hosts)==1,'exact peer reports')
                        p,h=proxies[0],hosts[0];check_remote(scenario,f,p,h)
                        require(child_settled(p['childPid'],p['childCreation']),'exact child not settled')
                        case.update(proxy=p,host=h,childSettled=True)
                    case['ok']=True
                except Exception as exc:
                    case['error']=str(exc)
                receipt['cases'].append(case)
        cases=receipt['cases']
        remote=[c for c in cases if c['label'].startswith('remote-')]
        references=[c for c in cases if c['label'].startswith('reference-')]
        receipt['remoteGatePassed']=len(remote)==2 and all(c['ok'] for c in remote)
        receipt['referenceLifecyclePassed']=len(references)==2 and all(c['ok'] for c in references)
        receipt['referenceImageMatched']=(len(cases)==4 and all(c['renderObservationVerified'] for c in cases)
            and all(len({c['fixture']['pixelOracle']['imageFnv'] if key=='imageFnv' else c['fixture'][key] for c in cases})==1
                    for key in ('imageFnv','vertexShaderFnv','pixelShaderFnv')))
        require(inputs==sources() and before==[identity(p) for p in inputs],'source drift during validation')
        require(identity(ref)==receipt['referenceDll'],'preserved reference changed')
        require(identity(build/'compile_commands.json')==receipt['compileCommands'], 'compile database drift during runtime')
        receipt['sourceAndReferencePreserved']=True
        require(receipt['remoteGatePassed'],'remote API/GPU/lifetime gate failed')
        require(receipt['referenceImageMatched'],'render/shader reference comparison incomplete or unequal')
        require(receipt['referenceLifecyclePassed'],'preserved reference lifecycle gate failed; image evidence is separate')
        receipt['ok']=True
    except Exception as exc:
        receipt['error']=str(exc)
    finally:
        with (out/'receipt.json').open('x',encoding='utf-8') as stream:json.dump(receipt,stream,ensure_ascii=False,indent=2)
    print(json.dumps(dict(ok=receipt['ok'],receipt=str(out/'receipt.json'),error=receipt.get('error')),ensure_ascii=False))
    return 0 if receipt['ok'] else 1


if __name__=='__main__':sys.exit(main())
