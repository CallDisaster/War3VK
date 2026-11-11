"""Freeze/build/run the real managed-author reset CPU path on x86 and x64.

Fresh artifacts only; serial BelowNormal children and finite timeouts. Reuses
the RH0 runner's identity/PE/invoke helpers. Production parser, registry, reset
and WeatherRuntime are linked, not mocked. WARVK_JAPI_PROTOCOL_TEST excludes
the pre-existing GPU/native backends, so this is NOT DLL, GPU or game proof.

The counterfactual is built only in a fresh source mirror: restore the old
reserve/push/clear reset and require its real bad_alloc termination precisely
at the populated shipping ResetAuthorState reserve. An observation-only TU
captures terminate and exits 86 to avoid a Windows crash dialog. It does not
implement, intercept or replace registry/reset/weather code. Arbitrary process
failure, timeout, a failed compile, or a normal test exit cannot kill this mutant.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

from run_render_host_protocol_gate import ROOT, identity, invoke, pe_identity


JAPI = 'src/d3d9/war3/japi/war3_japi_v1.cpp'
WEATHER = 'src/d3d9/war3/render/war3_weather_runtime.cpp'
RESET_TEST = 'AutoTest/test_war3_managed_object_reset.cpp'
WEATHER_TEST = 'AutoTest/test_warvk_weather_protocol.cpp'
JAPI_TEST = 'src/d3d9/war3/japi/tests/war3_japi_v1_protocol_test.cpp'
CODE_PATHS = (
    JAPI, WEATHER, RESET_TEST, WEATHER_TEST, JAPI_TEST,
    'src/d3d9/war3/japi/war3_japi_v1.h',
    'src/d3d9/war3/japi/war3_managed_object_reset.h',
    'src/d3d9/war3/render/war3_weather_runtime.h',
)
IDENTITY_PATHS = CODE_PATHS + (
    'AutoTest/run_managed_object_reset_gate.py',
    'AutoTest/run_render_host_protocol_gate.py',
    'src/d3d9/meson.build',
    'meson.build',
)
BEFORE_RESERVE = 'MANAGED_RESET_MUTANT_SHIPPING_RESET_BEFORE_RESERVE'
AFTER_RESERVE = 'MANAGED_RESET_MUTANT_SHIPPING_RESET_AFTER_RESERVE'
BAD_ALLOC = 'MANAGED_RESET_MUTANT_TERMINATED_WITH_BAD_ALLOC'
OTHER_TERMINATE = 'MANAGED_RESET_MUTANT_UNEXPECTED_TERMINATE'

TERMINATE_OBSERVER = r'''// Counterfactual-only termination observer. No production stubs.
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <new>
namespace {
[[noreturn]] void observeTerminate() noexcept {
  bool badAllocation = false;
  try {
    const auto active = std::current_exception();
    if (active) std::rethrow_exception(active);
  } catch (const std::bad_alloc&) {
    badAllocation = true;
  } catch (...) {
  }
  std::fputs(badAllocation
    ? "MANAGED_RESET_MUTANT_TERMINATED_WITH_BAD_ALLOC\n"
    : "MANAGED_RESET_MUTANT_UNEXPECTED_TERMINATE\n", stderr);
  std::fflush(stderr);
  std::fflush(stdout); // Preserve any earlier test failure before _Exit.
  std::_Exit(badAllocation ? 86 : 87);
}
struct InstallObserver {
  InstallObserver() { std::set_terminate(observeTerminate); }
} installObserver;
}
'''


def require(condition, message):
    if not condition:
        raise ValueError(message)


def write_new(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open('xb') as stream:
        stream.write(data)


def digest(data):
    return hashlib.sha256(data).hexdigest().upper()


def old_reset_counterfactual(data):
    """Change exactly one production function block; no test-body changes."""
    source = data.decode('utf-8')
    signature = 'void ResetAuthorState() noexcept {'
    require(source.count(signature) == 1, 'shipping reset signature must be unique')
    start = source.index(signature)
    end = source.index('\nvoid Reset() noexcept {', start)
    original = source[start:end]
    call = '  DetachAndRetireManagedObjects(g_objects, g_objectMutex,'
    require(original.count(call) == 1, 'current reset must call the real detachment helper once')
    call_start = original.index(call)
    call_end = original.index('  });', call_start) + len('  });')
    block = original[call_start:call_end]
    lambda_start = '[](const ManagedObject& object) noexcept {'
    require(block.count(lambda_start) == 1, 'retirement callback shape changed')
    callback = block[block.index(lambda_start) + len(lambda_start):block.rindex('  });')]
    newline = '\r\n' if '\r\n' in block else '\n'
    callback = callback.replace('\r\n', '\n').strip('\n')
    replacement = '''  // MUTATION: restore old allocating vector reset, only in this mirror.
  std::vector<ManagedObject> objects;
  {
    std::lock_guard<std::mutex> lock(g_objectMutex);
    const bool mutationHasObjects = !g_objects.empty();
    if (mutationHasObjects) {
      std::fputs("MANAGED_RESET_MUTANT_SHIPPING_RESET_BEFORE_RESERVE\\n", stderr);
      std::fflush(stderr);
    }
    objects.reserve(g_objects.size());
    if (mutationHasObjects) {
      std::fputs("MANAGED_RESET_MUTANT_SHIPPING_RESET_AFTER_RESERVE\\n", stderr);
      std::fflush(stderr);
    }
    for (const auto& entry : g_objects)
      objects.push_back(entry.second);
    g_objects.clear();
  }
  for (const ManagedObject& object : objects) {
CALLBACK_BODY
  }'''.replace('CALLBACK_BODY', callback).replace('\n', newline)
    modified = original[:call_start] + replacement + original[call_end:]
    require('ResetTypedTransport();' in modified,
            'mutation must retain actual typed transport reset')
    require('render::WeatherRuntime::instance().resetAuthorState();' in modified,
            'mutation must retain actual weather reset')
    require('DetachAndRetireManagedObjects' not in modified,
            'old reset mutant must not call new detachment helper')
    mutated = '#include <cstdio>' + newline + source[:start] + modified + source[end:]
    return mutated.encode('utf-8'), {
        'name': 'old-reserve-push-noexcept-reset',
        'changedProductionFunction': 'dxvk::war3::japi::ResetAuthorState',
        'originalFunctionSha256': digest(original.encode('utf-8')),
        'mutatedFunctionSha256': digest(modified.encode('utf-8')),
        'testBodyUnchanged': True,
        'beforeReserveMarker': BEFORE_RESERVE,
        'afterReserveMarker': AFTER_RESERVE,
        'expectedExitCode': 86,
        'scope': 'mirror-only; actual std::bad_alloc escapes the old noexcept reset',
    }


def invocation_record(argv, timeout):
    """Successful builds/runs use the established BelowNormal finite helper."""
    stdout, stderr = invoke(argv, timeout)
    return {'argv': list(map(str, argv)), 'timeoutSeconds': timeout,
            'stdout': stdout, 'stderr': stderr, 'exitCode': 0}


def run_expected_termination(exe, timeout):
    # invoke() deliberately throws for nonzero exit. Preserve the same process
    # policy here but keep the actual return code for this one negative oracle.
    flags = 0
    if os.name == 'nt':
        flags = subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS
    result = subprocess.run([str(exe)], cwd=ROOT, capture_output=True, text=True,
                            encoding='utf-8', errors='replace', timeout=timeout,
                            creationflags=flags)
    return {'argv': [str(exe)], 'timeoutSeconds': timeout,
            'stdout': result.stdout.strip(), 'stderr': result.stderr.strip(),
            'exitCode': result.returncode}


def require_current_output(name, result):
    require(not result['stderr'], f'{name}: unexpected stderr: {result["stderr"]}')
    if name == 'managed-reset':
        match = re.fullmatch(r'managed reset checks=(\d+) failures=0 deniedAllocationAttempts=(\d+)',
                             result['stdout'])
        require(match is not None, 'managed reset: unexpected or failed test output')
        require(int(match[1]) >= 20, 'managed reset: substantial production checks missing')
        require(int(match[2]) == 1,
                'managed reset: allocation injection inactive or reset attempted allocation')
        return {'checks': int(match[1]), 'failures': 0, 'deniedAllocationAttempts': int(match[2])}
    if name == 'weather-protocol':
        match = re.fullmatch(r'weather_protocol: (\d+) checks, 0 failures', result['stdout'])
        require(match is not None and int(match[1]) >= 100,
                'weather protocol: unexpected, vacuous or failed test output')
        return {'checks': int(match[1]), 'failures': 0}
    require(name == 'japi-protocol' and result['stdout'] == 'WarVK JAPI v1 protocol tests passed',
            'JAPI protocol: unexpected or failed test output')
    return {'checks': None, 'failures': 0,
            'note': 'existing JAPI executable reports success without an assertion count'}


def require_mutant_killed(result):
    require(result['exitCode'] == 86,
            f'old reset did not reach observed bad_alloc termination: exit={result["exitCode"]}')
    require(result['stderr'].splitlines() == [BEFORE_RESERVE, BAD_ALLOC],
            'counterfactual failed elsewhere or passed reserve; shipping-reset evidence missing')
    require(not result['stdout'], 'counterfactual printed prior test failures or a normal completion')
    require(AFTER_RESERVE not in result['stderr'] and OTHER_TERMINATE not in result['stderr'],
            'counterfactual termination did not stop at the allocating reserve')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler32', type=Path, required=True)
    parser.add_argument('--compiler64', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--build-timeout', type=int, default=180)
    parser.add_argument('--run-timeout', type=int, default=30)
    args = parser.parse_args()
    require(1 <= args.build_timeout <= 600 and 1 <= args.run_timeout <= 120,
            'finite build/run timeouts required')
    compilers = {32: args.compiler32.resolve(strict=True), 64: args.compiler64.resolve(strict=True)}
    require(all(path.is_file() for path in compilers.values()), 'compiler paths must be files')
    output = args.output.resolve()
    output.mkdir(parents=False, exist_ok=False)
    receipt = {
        'schema': 1, 'scope': 'MANAGED_AUTHOR_RESET_REAL_CPU_PATH_ONLY', 'ok': False,
        'shippingDllBuilt': False, 'gpuTested': False, 'gameLaunched': False,
        'deployed': False, 'visualAccepted': False,
        'realParserRegistryResetWeather': True,
        'excludedByExistingProtocolTestMode': 'native/GPU backends (not registry/reset/weather)',
        'source': [], 'frozenSource': [], 'counterfactualSource': [],
        'builds': [], 'runs': [], 'mutations': [],
        'sourceUnchangedAfter': False,
        'frozenSourceUnchangedAfter': False, 'counterfactualSourceUnchangedAfter': False,
    }
    try:
        receipt['source'] = [identity(ROOT / relative) for relative in IDENTITY_PATHS]
        frozen = output / 'source'
        mutant = output / 'old-reset-source'
        frozen.mkdir()
        mutant.mkdir()
        baseline = {entry['path']: entry['sha256'] for entry in receipt['source']}
        mutation = None
        for relative in CODE_PATHS:
            raw = (ROOT / relative).read_bytes()
            require(digest(raw) == baseline[str(ROOT / relative)],
                    f'source changed while freezing: {relative}')
            write_new(frozen / relative, raw)
            receipt['frozenSource'].append(identity(frozen / relative))
            if relative == JAPI:
                raw, mutation = old_reset_counterfactual(raw)
            write_new(mutant / relative, raw)
        observer = mutant / 'AutoTest/reset_terminate_observer.cpp'
        write_new(observer, TERMINATE_OBSERVER.encode('utf-8'))
        mutation['source'] = identity(mutant / JAPI)
        mutation['terminateObserver'] = identity(observer)
        mutation['unchangedProductionTest'] = identity(mutant / RESET_TEST)
        require(identity(mutant / RESET_TEST)['sha256'] == identity(frozen / RESET_TEST)['sha256'],
                'counterfactual must use byte-identical production test')
        receipt['mutationDefinition'] = mutation
        receipt['counterfactualSource'] = [identity(mutant / relative) for relative in CODE_PATHS]
        receipt['counterfactualSource'].append(identity(observer))
        specs = (('managed-reset', RESET_TEST), ('weather-protocol', WEATHER_TEST), ('japi-protocol', JAPI_TEST))
        for bits, compiler in compilers.items():
            compiler_before = identity(compiler)
            expected_pe = (0x14c, 0x10b) if bits == 32 else (0x8664, 0x20b)
            common = [compiler, '-std=c++17', '-O2', '-Wall', '-Wextra', '-static', '-pthread',
                      '-DWARVK_JAPI_PROTOCOL_TEST=1']
            for name, test in specs:
                exe = output / f'{name}-{bits}.exe'
                argv = common + ['-I', frozen, frozen / JAPI, frozen / WEATHER, frozen / test, '-o', exe]
                build = invocation_record(argv, args.build_timeout)
                require(pe_identity(exe) == expected_pe, f'{name}: requested {bits}-bit PE identity differs')
                build.update({'name': name, 'bits': bits, 'compiler': compiler_before, 'exe': identity(exe)})
                receipt['builds'].append(build)
                result = invocation_record([exe], args.run_timeout)
                result.update(require_current_output(name, result))
                result.update({'name': name, 'bits': bits, 'exe': identity(exe)})
                receipt['runs'].append(result)
            exe = output / f'managed-reset-old-reserve-{bits}.exe'
            argv = common + ['-I', mutant, mutant / JAPI, mutant / WEATHER,
                             mutant / RESET_TEST, observer, '-o', exe]
            build = invocation_record(argv, args.build_timeout)
            require(pe_identity(exe) == expected_pe, f'mutant: requested {bits}-bit PE identity differs')
            build.update({'name': 'old-reset-counterfactual', 'bits': bits,
                          'compiler': compiler_before, 'exe': identity(exe)})
            receipt['builds'].append(build)
            result = run_expected_termination(exe, args.run_timeout)
            result.update({'bits': bits, 'exe': identity(exe), 'verdict': 'NOT_YET_KILLED'})
            receipt['mutations'].append(result)
            require_mutant_killed(result)
            result['verdict'] = 'KILLED_AT_REAL_SHIPPING_RESET_RESERVE'
            require(identity(compiler) == compiler_before, f'{bits}-bit compiler changed during gate')
        after = [identity(ROOT / relative) for relative in IDENTITY_PATHS]
        receipt['sourceUnchangedAfter'] = after == receipt['source']
        require(receipt['sourceUnchangedAfter'], 'live source identity changed during gate; no combined certification')
        receipt['frozenSourceUnchangedAfter'] = (
            [identity(frozen / relative) for relative in CODE_PATHS] == receipt['frozenSource'])
        receipt['counterfactualSourceUnchangedAfter'] = (
            [identity(mutant / relative) for relative in CODE_PATHS] + [identity(observer)]
            == receipt['counterfactualSource'])
        require(receipt['frozenSourceUnchangedAfter'] and receipt['counterfactualSourceUnchangedAfter'],
                'artifact source mirror changed while compiling/running')
        require(len(receipt['runs']) == 6 and len(receipt['mutations']) == 2,
                'both widths and both real counterfactuals are mandatory')
        receipt['ok'] = True
    except Exception as exc:
        receipt['error'] = str(exc)
    finally:
        with (output / 'receipt.json').open('x', encoding='utf-8', newline='\n') as stream:
            json.dump(receipt, stream, ensure_ascii=False, indent=2)
            stream.write('\n')
    print(json.dumps({'ok': receipt['ok'], 'receipt': str(output / 'receipt.json'),
                      'runs': len(receipt['runs']), 'mutations': len(receipt['mutations']),
                      'sourceUnchangedAfter': receipt['sourceUnchangedAfter'],
                      'error': receipt.get('error')}, ensure_ascii=False))
    return 0 if receipt['ok'] else 1


if __name__ == '__main__':
    sys.exit(main())
