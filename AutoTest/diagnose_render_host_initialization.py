"""Diagnostic-only copy-TU timing of the frozen Presenter r10 clear-soak.

Default: bind inputs and prepare a copied source, no build/native launch.
Root's sole build/native lease plus --execute-native are required to compile
and run. This is NEVER an acceptance gate: acceptancePassed is always false.
The historical environment was not recorded. We reproduce the frozen runner's
safe control recipe, not its complete historical environment or driver state.
"""
from __future__ import annotations

import argparse
import difflib
import json
import os
from pathlib import Path
import re
import shutil
import sys


# Works in the draft artifact directory AND after moving into AutoTest. Never
# import helpers through an imaginary parent of the draft directory.
ROOT = next(p for p in Path(__file__).resolve().parents
            if (p/'AutoTest/run_render_host_memory_destroy_mutation.py').is_file()
            and (p/'tools/render_host/dxvk_core/d3d9_backend.cpp').is_file())
sys.path.insert(0, str(ROOT/'AutoTest'))
import run_render_host_memory_destroy_mutation as copied
from run_render_host_protocol_gate import identity, pe_identity
from run_render_host_transport_gate import require, strict_json, child_settled
from run_render_host_core_gate import execute, check_host_compile_boundary
from run_render_host_d3d9_gate import json_lines, native_uint
from run_render_host_presenter_gate import check_launch
from isolated_render_desktop import IsolatedRenderDesktop, validate_probe_receipt
from render_lab_raw_artifacts import capture_raw_artifacts, verify_raw_artifacts, raw_manifests

BASELINE = ROOT/'AutoTest/artifacts/x64-presenter-20261022-r10/receipt.json'
BASELINE_SHA = 'A05E4F041FB365F7D4BA8DFEA71B6DF633C5B65AA787B819ED9020802F82FB35'
HOST_SHA = '888B801C6587AAFB4873B30A9FE048C0CC2F575EFE4D25E6A4E143F72DD3B4C3'
BACKEND = copied.BACKEND
TARGET = copied.TARGET
PREFIX = 'WARVK_INIT_STAGE_V1'
RECIPE = ROOT/'AutoTest/run_render_host_presenter_gate.py'
ENV_ALLOW = frozenset(('SYSTEMROOT', 'WINDIR', 'COMSPEC', 'PATH', 'TEMP', 'TMP',
    'USERPROFILE', 'APPDATA', 'LOCALAPPDATA', 'PROGRAMFILES', 'PROGRAMFILES(X86)',
    'PROGRAMDATA', 'NUMBER_OF_PROCESSORS', 'PROCESSOR_ARCHITECTURE'))
OUTER_PHASES = ('instance', 'adapter-check', 'device', 'context-owner-resources',
    'presenter-surface', 'presenter-swapchain', 'blitter', 'presented-readback',
    'first-submit-retire')
SUBMIT_PHASES = ('cpu-submit', 'gpu-timeline', 'resource-retire')
RAW_NAMES = ['case.stdout', 'case.stderr', 'host.log']


TRACE_CPP = r'''
  // DIAGNOSTIC COPY TU ONLY. Fixed initialization scope, no wire changes.
  struct InitDiagnosticTrace;
  thread_local InitDiagnosticTrace* initDiagnosticTrace = nullptr;
  struct InitDiagnosticTrace {
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    InitDiagnosticTrace* previous = initDiagnosticTrace;
    uint64_t priorLogNs = 0;
    uint64_t creation100ns = 0;
    uint32_t index = 0;
    int exceptions = std::uncaught_exceptions();
    bool inSubmit = false;
    void emit(const char* phase, const char* event) noexcept {
      const auto before = std::chrono::steady_clock::now();
      const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(before - start).count();
      std::fprintf(stderr, "WARVK_INIT_STAGE_V1 pid=%lu tid=%lu creation100ns=%llu index=%u phase=%s event=%s elapsedNs=%llu priorLogNs=%llu\n",
        GetCurrentProcessId(), GetCurrentThreadId(), static_cast<unsigned long long>(creation100ns), ++index, phase, event,
        static_cast<unsigned long long>(elapsed), static_cast<unsigned long long>(priorLogNs));
      std::fflush(stderr);
      priorLogNs += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - before).count());
    }
    InitDiagnosticTrace() noexcept {
      initDiagnosticTrace = this;
      FILETIME creation{}, exit{}, kernel{}, user{};
      if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user))
        creation100ns = (static_cast<uint64_t>(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
      emit("initialize", "begin");
    }
    ~InitDiagnosticTrace() noexcept {
      emit("initialize", std::uncaught_exceptions() > exceptions ? "exception" : "end");
      initDiagnosticTrace = previous;
    }
  };
  struct InitDiagnosticStage {
    InitDiagnosticTrace* trace;
    const char* phase;
    int exceptions = std::uncaught_exceptions();
    InitDiagnosticStage(const char* name, bool submitOnly = false) noexcept
      : trace(initDiagnosticTrace && (!submitOnly || initDiagnosticTrace->inSubmit)
          ? initDiagnosticTrace : nullptr), phase(name) {
      if (trace) trace->emit(phase, "begin");
    }
    void end() noexcept {
      if (trace) { trace->emit(phase, "end"); trace = nullptr; }
    }
    ~InitDiagnosticStage() noexcept {
      if (trace) trace->emit(phase,
        std::uncaught_exceptions() > exceptions ? "exception" : "end");
    }
  };
  struct InitDiagnosticSubmitScope {
    InitDiagnosticTrace* trace = initDiagnosticTrace;
    bool previous = trace ? trace->inSubmit : false;
    InitDiagnosticSubmitScope() noexcept { if (trace) trace->inSubmit = true; }
    ~InitDiagnosticSubmitScope() noexcept { if (trace) trace->inSubmit = previous; }
  };
'''


def instrument_source(original):
    """Insertion-only exact-anchor transform; original statements stay in order.

    Do not wrap production locals in new scopes (which could shorten ownership).
    Return insertion receipts so the pure test can reconstruct the original TU.
    """
    require(type(original) is str and PREFIX not in original, 'one uninstrumented source required')
    require('\r' not in original, 'normalize newlines before instrumentation')
    edits = []
    def add(text, anchor, before='', after=''):
        require(text.count(anchor) == 1, 'exact single instrumentation anchor: '+anchor[:100])
        require(bool(before) != bool(after), 'one insertion side required')
        insertion = before or after
        edits.append(dict(anchor=anchor, side='before' if before else 'after', insertion=insertion))
        return text.replace(anchor, before+anchor+after, 1)
    source = add(original, '  using namespace dxvk;\n', before=TRACE_CPP+'\n')
    start = source.index('void D3D9Backend::initialize(')
    end = source.index('\nvoid D3D9Backend::createBuffer(', start)
    init = source[start:end]
    init = add(init, '  try {\n', before='  InitDiagnosticTrace initTiming;\n')
    phases = (
        ('instance', '    owner.instance = new DxvkInstance(DxvkInstanceFlag::ClientApiIsD3D9);\n',
            '    owner.adapter = owner.instance->enumAdapters(0);\n'),
        ('adapter-check', '    owner.adapter = owner.instance->enumAdapters(0);\n',
            '    owner.device = owner.adapter->createDevice();\n'),
        ('device', '    owner.device = owner.adapter->createDevice();\n',
            '    owner.context = owner.device->createContext();\n'),
        ('context-owner-resources', '    owner.context = owner.device->createContext();\n',
            '    if (window != nullptr) {\n      owner.colorSampleView'),
        ('presenter-surface', '      owner.presenter = new Presenter(',
            '      owner.presenter->setSurfaceExtent({64, 64});\n'),
        ('presenter-swapchain', '      owner.presenter->setSurfaceExtent({64, 64});\n',
            '      owner.blitter = new DxvkSwapchainBlitter(owner.device, nullptr);\n'),
        ('blitter', '      owner.blitter = new DxvkSwapchainBlitter(owner.device, nullptr);\n',
            '      if (diagnosticPresentedReadback) {\n'),
        ('presented-readback', '        owner.presentedReadback = owner.coherentBuffer(',
            '        owner.presentedBytes.resize(PresentedBytes);\n'),
        ('first-submit-retire', '    DxvkFenceCreateInfo fenceInfo{};\n',
            '    owner.submit(m_stats, true);\n'),
    )
    for phase, begin, finish in phases:
        var = 'init_'+phase.replace('-', '_')
        indent = begin[:len(begin)-len(begin.lstrip())]
        init = add(init, begin, before=f'{indent}InitDiagnosticStage {var}("{phase}");\n')
        if phase in ('presented-readback', 'first-submit-retire'):
            init = add(init, finish, after=f'{indent}{var}.end();\n')
        else:
            init = add(init, finish, before=f'{indent}{var}.end();\n')
    source = source[:start]+init+source[end:]
    start = source.index('  void submit(Stats& stats, bool waitForGpu) {\n')
    end = source.index('\n  Rc<DxvkBuffer> coherentBuffer(', start)
    submit = source[start:end]
    submit = add(submit, '    canAdd(lastSerial, 1);\n', before=
        '    InitDiagnosticSubmitScope initSubmitScope;\n'
        '    InitDiagnosticStage init_cpu_submit("cpu-submit", true);\n')
    submit = add(submit, '    if (waitForGpu)\n', before='    init_cpu_submit.end();\n')
    source = source[:start]+submit+source[end:]
    start = source.index('  void settle(Stats& stats) {\n')
    end = source.index('\n  void waitPresentRetired(', start)
    settle = source[start:end]
    settle = add(settle, '    const VkResult result = device->vkd()->vkWaitSemaphores(', before=
        '    InitDiagnosticStage init_gpu_timeline("gpu-timeline", true);\n')
    settle = add(settle, '    stats.gpuCompleted = lastSerial;\n', before='    init_gpu_timeline.end();\n')
    source = source[:start]+settle+source[end:]
    start = source.index('  void waitRetired() {\n')
    end = source.index('\n  uint64_t pollTimeline(', start)
    retire = source[start:end]
    retire = add(retire, '  void waitRetired() {\n', after=
        '    InitDiagnosticStage init_resource_retire(initDiagnosticTrace && initDiagnosticTrace->inSubmit\n'
        '      ? "resource-retire" : "resource-retire-pre-submit");\n')
    # RAII closes the last scope, including exception paths, without touching
    # the original healthy() or loop. No per-frame events outside initialize.
    source = source[:start]+retire+source[end:]
    return source, edits


def expected_events(*, readback=True):
    require(type(readback) is bool, 'explicit readback profile bool required')
    events = [('initialize', 'begin')]
    for phase in OUTER_PHASES:
        if phase == 'presented-readback' and not readback:
            continue
        events.append((phase, 'begin'))
        if phase == 'first-submit-retire':
            events.extend((('resource-retire-pre-submit', 'begin'), ('resource-retire-pre-submit', 'end')))
            for nested in SUBMIT_PHASES:
                events.extend(((nested, 'begin'), (nested, 'end')))
        events.append((phase, 'end'))
    return events+[('initialize', 'end')]


MARKER_RE = re.compile(r'WARVK_INIT_STAGE_V1 pid=([1-9][0-9]*) tid=([1-9][0-9]*) creation100ns=([1-9][0-9]*) '
    r'index=([1-9][0-9]*) phase=([a-z-]+) event=(begin|end|exception) '
    r'elapsedNs=([0-9]+) priorLogNs=([0-9]+)')


def read_stages(text, expected_pid=None, expected_tid=None, expected_creation=None, *, readback=True):
    """Strict complete ordered trace, or ordered exception unwind; never PASS."""
    require(type(text) is str, 'stage input text required')
    require(all(value is None or native_uint(value, 32) and value > 0
                for value in (expected_pid, expected_tid)), 'strict expected native marker identity')
    require(expected_creation is None or native_uint(expected_creation) and expected_creation > 0,
            'strict expected host creation identity')
    rows = []
    for line in text.splitlines():
        if PREFIX not in line:
            continue
        match = MARKER_RE.fullmatch(line)
        require(match is not None, 'malformed initialization marker')
        pid, tid, creation, index, phase, event, elapsed, log = match.groups()
        rows.append(dict(pid=int(pid), tid=int(tid), creation100ns=int(creation), index=int(index), phase=phase,
                         event=event, elapsedNs=int(elapsed), priorLogNs=int(log)))
    success = expected_events(readback=readback)
    require(rows and len(rows) <= len(success), 'missing or excessive initialization markers')
    stack = []; intervals = []; cursor = 0; unwinding = False
    previous_time = previous_log = 0
    for index, row in enumerate(rows, 1):
        require(row['index'] == index, 'duplicate, missing or reordered marker index')
        require(0 < row['pid'] <= 0xffffffff and 0 < row['tid'] <= 0xffffffff
                and 0 < row['creation100ns'] <= 0xffffffffffffffff, 'marker native identity range')
        require(all(row[k] == rows[0][k] for k in ('pid', 'tid', 'creation100ns')), 'mixed initialization identity')
        for name, expected in (('pid', expected_pid), ('tid', expected_tid), ('creation100ns', expected_creation)):
            require(expected is None or row[name] == expected, 'marker '+name+' differs from native witness')
        require(previous_time <= row['elapsedNs'] <= 0xffffffffffffffff
                and previous_log <= row['priorLogNs'] <= row['elapsedNs']
                and row['priorLogNs']-previous_log <= row['elapsedNs']-previous_time,
                'nonmonotonic/impossible marker clocks')
        previous_time, previous_log = row['elapsedNs'], row['priorLogNs']
        pair = row['phase'], row['event']
        if row['event'] == 'exception':
            unwinding = True
            require(stack and stack[-1]['phase'] == row['phase'], 'exception does not close active phase')
        else:
            require(not unwinding and cursor < len(success) and pair == success[cursor],
                    'missing, unexpected or reordered initialization phase')
            cursor += 1
        if row['event'] == 'begin':
            stack.append(row)
        else:
            require(stack and stack[-1]['phase'] == row['phase'], 'unmatched initialization phase')
            begin = stack.pop()
            intervals.append(dict(phase=row['phase'], terminal=row['event'],
                inclusiveNs=row['elapsedNs']-begin['elapsedNs'],
                loggingNsBetweenMarkers=row['priorLogNs']-begin['priorLogNs']))
    require(not stack and rows[-1]['phase'] == 'initialize' and rows[-1]['event'] in ('end', 'exception'),
            'incomplete initialization trace; preserve raw evidence')
    require(unwinding or cursor == len(success), 'incomplete successful initialization trace')
    return dict(acceptancePassed=False, traceComplete=True, initializationSucceeded=not unwinding,
        pid=rows[0]['pid'], tid=rows[0]['tid'], creation100ns=rows[0]['creation100ns'], markerCount=len(rows), intervals=intervals,
        measuredPriorLoggingNs=rows[-1]['priorLogNs'], finalMarkerLoggingNotMeasured=True,
        timerCallOverheadNotSeparated=True, rows=rows)


def recipe_environment(current, case_dir, host):
    """Only historical safe allowlist + unchanged renderer controls; no tokens."""
    require(type(current) is dict and all(type(k) is str and type(v) is str for k, v in current.items()),
            'environment must be a string map')
    safe = {k.upper(): v for k, v in current.items() if k.upper() in ENV_ALLOW}
    require(len(safe) == sum(k.upper() in ENV_ALLOW for k in current), 'case-duplicate environment names')
    safe.update(DXVK_LOG_LEVEL='info', DXVK_SHADER_CACHE='0', DXVK_LOG_PATH=str(case_dir),
        DXVK_CONFIG_FILE=str(case_dir/'absent.conf'), WARVK_D3D9_PROFILE='presenter-readback-isolated-v1',
        WARVK_D3D9_HOST_EXE=str(host), WARVK_D3D9_HOST_LOG=str(case_dir/'host.log'))
    return safe


def baseline_case(receipt):
    require(receipt['scope'] == 'D3D9_DXVK_PRESENTER_NONINPUT_DESKTOP' and receipt['ok'] is False
            and receipt['restrictedMediumRequested'] is True, 'exact red restricted-medium r10 scope')
    cases = [c for c in receipt['cases'] if c['label'] == 'clear-soak']
    require(len(cases) == 1, 'one historical clear-soak case required')
    case = cases[0]; process = case['process']; argv = process['argv']
    require(type(argv) is list and len(argv) == 11 and all(type(a) is str for a in argv), 'exact r10 argv shape')
    require(argv[1] == '--dll' and argv[3:8] == ['--scenario', 'clear-soak', '--diagnostic-readback', '1', '--expect-desktop']
            and argv[9] == '--input-desktop', 'r10 scenario/profile flags changed')
    require(argv[8] == process['desktop'] and argv[10] == process['inputBefore']
            and process['timeoutSeconds'] == 45 and type(process['timeoutSeconds']) in (int, float), 'r10 deadline/Desktop')
    require(process['restrictedToken']['mode'] == 'restricted-medium-per-child/v1'
            and process['restrictedToken']['childTokenVerifiedBeforeResume'] is True, 'r10 actual restricted token missing')
    require(Path(argv[0]).name == 'd3d9-presenter-fault32.exe' and Path(argv[2]).name == 'proxy.dll', 'r10 fixture/DLL identity')
    return case


def replay_argv(original, fixture, proxy, desktop, input_desktop):
    changed = list(original)
    for slot, value in ((0, fixture), (2, proxy), (8, desktop), (10, input_desktop)):
        changed[slot] = str(value)
    require(all(original[i] == changed[i] for i in range(len(original)) if i not in (0, 2, 8, 10)),
            'unexpected scenario flag substitution')
    return changed


def bind_baseline():
    """Bind r10 source/compiler/host, not a successful M0 acceptance receipt."""
    require(identity(BASELINE)['sha256'] == BASELINE_SHA, 'frozen r10 receipt changed')
    old = strict_json(BASELINE.read_text('utf-8')); case = baseline_case(old)
    # Only the changing Python test inventory may differ; all actual C/C++,
    # build inputs and the exact environment recipe must still be from r10.
    source_rows = [r for r in old['sources'] if not Path(r['path']).is_relative_to(ROOT/'AutoTest')
                   or Path(r['path']).suffix != '.py' or Path(r['path']) == RECIPE]
    require(source_rows and any(Path(r['path']) == BACKEND for r in source_rows)
            and any(Path(r['path']) == RECIPE for r in source_rows), 'r10 TU/runner source identities missing')
    frozen = source_rows+old['compilers']+old['binaries']+[old['compileCommands'], identity(BASELINE)]
    frozen += [old[name] for name in ('playerBefore', 'referenceBefore')]
    for pair in old['caseCopies']:
        require(all(pair['source'][k] == pair['copy'][k] for k in ('bytes', 'sha256')), 'historical binary copy differs')
        frozen.extend((pair['source'], pair['copy']))
    for row in frozen:
        copied.same_file(row)
    for raw in raw_manifests(old):
        verify_raw_artifacts(raw)
    build = Path(old['compileCommands']['path']).parent
    require(identity(build/TARGET)['sha256'] == HOST_SHA, 'current host is not r10 host')
    require('inline constexpr DWORD DeadlineMs=5000;' in (ROOT/'tools/render_host/win32_transport.h').read_text('utf-8'),
            'original transport 5s gate changed')
    require(not (Path(case['process']['stdout']).parent/'absent.conf').exists(), 'r10 absent config now exists')
    return old, case, build, frozen


def freeze_link_inputs(build, link, query, compiler, out, env, receipt):
    """Reuse existing exact Ninja/llvm-ar member readers, not archive rewrites."""
    rows = [identity(p) for p in copied.declared_link_inputs(query, build, link)]
    archiver = compiler.parent/'llvm-ar.exe'; rows.append(identity(archiver))
    receipt['archiver'] = rows[-1]; archives = []; members = set()
    for row in list(rows[:-1]):
        path = Path(row['path'])
        if path.suffix.lower() not in ('.a', '.lib'):
            continue
        with path.open('rb') as stream:
            magic = stream.read(8)
        require(magic in (b'!<arch>\n', b'!<thin>\n'), 'unmodelled archive format')
        if magic != b'!<thin>\n':
            continue
        require(path.is_relative_to(build), 'thin archive outside original build')
        stem = f'thin-{len(archives):02d}'
        for suffix, argv, cwd in (
            ('query', ['ninja', '-C', build, '-t', 'query', path.relative_to(build).as_posix()], ROOT),
            ('members', [archiver, 't', path.relative_to(build).as_posix()], build)):
            run_step(argv, cwd, out/(stem+'-'+suffix), 30, env, receipt)
        actual = copied.thin_archive_members((out/(stem+'-query.stdout')).read_text('utf-8'),
            (out/(stem+'-members.stdout')).read_text('utf-8'), build, path)
        archives.append(dict(archive=row, members=list(map(str, actual)))); members.update(actual)
    rows.extend(identity(p) for p in sorted(members))
    receipt['thinArchives'] = archives
    return rows


def run_step(argv, cwd, prefix, seconds, env, receipt):
    """Freeze complete compiler/query output even when execute() raises."""
    try:
        receipt['steps'].append(execute(argv, cwd, prefix, seconds, env))
    finally:
        receipt['stepRawArtifacts'].append(capture_raw_artifacts(prefix.parent,
            [prefix.with_suffix('.stdout').name, prefix.with_suffix('.stderr').name]))


def build_diagnostic(old, build, source, out, env, receipt, frozen):
    check_host_compile_boundary(build)
    for name, argv in (
        ('original-no-work', ['ninja', '-C', build, '-n', TARGET.as_posix()]),
        ('original-commands', ['ninja', '-C', build, '-t', 'commands', TARGET.as_posix()]),
        ('original-query', ['ninja', '-C', build, '-t', 'query', TARGET.as_posix()])):
        run_step(argv, ROOT, out/name, 30, env, receipt)
    require('no work to do' in (out/'original-no-work.stdout').read_text('utf-8'), 'original build not frozen')
    entries = [e for e in strict_json((build/'compile_commands.json').read_text('utf-8'))
               if copied.path_arg(e['file'], e['directory']) == BACKEND]
    require(len(entries) == 1, 'one exact backend compile entry')
    obj = source.with_suffix('.cpp.obj'); dep = source.with_suffix('.cpp.obj.d')
    compile = copied.compile_plan(entries[0], source, obj, dep)
    require(Path(compile['cwd']) == build.resolve(), 'original compiler cwd changed')
    compiler = copied.path_arg(compile['argv'][0], build)
    require(identity(compiler) in old['compilers'], 'compiler not frozen in r10')
    host = out/'warvk-d3d9-host-dev.exe'
    link = copied.link_plan((out/'original-commands.stdout').read_text('utf-8'), build,
                           Path(compile['originalObject']), obj, host)
    require(copied.path_arg(link['argv'][0], build) == compiler, 'linker driver identity changed')
    frozen.extend(freeze_link_inputs(build, link, (out/'original-query.stdout').read_text('utf-8'),
                                    compiler, out, env, receipt))
    frozen.append(identity(build/'build.ninja'))
    receipt.update(compilePlan=compile, linkPlan=link, implicitToolchainLibrariesIndividuallyMeasured=False)
    for row in frozen:
        copied.same_file(row)
    run_step(compile['argv'], build, out/'diagnostic-compile', 180, env, receipt)
    run_step(link['argv'], build, out/'diagnostic-link', 180, env, receipt)
    require(pe_identity(host) == (0x8664, 0x20b), 'real x64 diagnostic host required')
    receipt.update(diagnosticObject=identity(obj), diagnosticHost=identity(host))
    for row in frozen:
        copied.same_file(row)
    return host


def check_diagnostic_settlement(launch, desktop, input_desktop):
    """A failed fixture may yield timings, but timeout/kill/uncertain Job may not."""
    require(launch['exitCode'] in (0, 2) and type(launch['exitCode']) is int, 'unexpected fixture exit')
    require(launch['jobActiveProcesses'] == 0 and type(launch['jobActiveProcesses']) is int,
            'diagnostic Job not empty')
    for name in ('processSettled', 'jobEmpty', 'jobAtomic', 'handleListApplied'):
        require(launch[name] is True, 'missing diagnostic settlement '+name)
    for name in ('timedOut', 'forcedTermination', 'desktopHandleInherited'):
        require(launch[name] is False, 'failed diagnostic settlement '+name)
    require(launch['cleanupErrors'] == [] and launch['desktop'] == desktop
            and launch['inputBefore'] == launch['inputAfter'] == input_desktop
            and desktop != input_desktop, 'diagnostic cleanup/Desktop')
    require(all(native_uint(launch[key], bits) and launch[key] > 0 for key, bits in
        (('pid', 32), ('initialThreadId', 32), ('creation100ns', 64))), 'exact diagnostic parent identity')
    require(type(launch['argv']) is list and launch['argv'] and type(launch['argv'][0]) is str
            and Path(launch['actualExecutable']).resolve() == Path(launch['argv'][0]).resolve()
            and launch['timeoutSeconds'] == 45, 'actual fixture image/deadline changed')
    token = launch['restrictedToken']
    require(token['mode'] == 'restricted-medium-per-child/v1' and token['childTokenVerifiedBeforeResume'] is True
            and token['derivedTokenClosed'] is True and token['cleanupErrors'] == [], 'diagnostic medium-token witness')
    require(token['resumedAfterVerification'] is True and type(token['resumePreviousCount']) is int
            and token['resumePreviousCount'] == 1 and token['parentTokenUnchanged'] is True
            and token['noHighIntegrityFallback'] is True and token['globalSecurityChanged'] is False,
            'actual restricted child verification/resume contract')
    observed = token['childBeforeResume']
    require(type(observed['integrityRid']) is int and observed['integrityRid'] == 8192
            and observed['integritySid'] == 'S-1-16-8192' and observed['hasRestrictions'] is True,
            'actual suspended child not restricted Medium')


def read_bound_stages(text, hosts, reports, launch, desktop, input_desktop, *, readback=True):
    """Bind marker PID+creation to actual proxy child and authenticated host."""
    witnesses = [r for r in hosts if r.get('schema') == 'warvk-d3d9-desktop/v1']
    proxies = [r for r in reports if r.get('schema') == 'warvk-d3d9-proxy/v1']
    require(len(witnesses) == len(proxies) == 1, 'one host witness and one actual proxy child required')
    witness, proxy = witnesses[0], proxies[0]
    require(witness['beforeGpu'] is True and witness['desktop'] == desktop
            and witness['inputDesktop'] == input_desktop and witness['windowStation'] == 'WinSta0'
            and native_uint(witness['peerPid'], 32) and witness['peerPid'] == launch['pid'],
            'host Desktop/authenticated parent mismatch')
    require(native_uint(proxy['childPid'], 32) and proxy['childPid'] > 0
            and native_uint(proxy['childCreation']) and proxy['childCreation'] > 0
            and proxy['peerVerified'] is True and proxy['x86VulkanAbsent'] is True,
            'exact authenticated native proxy child identity')
    require(witness['pid'] == proxy['childPid'] and native_uint(witness['windowThreadId'], 32)
            and witness['windowThreadId'] > 0, 'host/window owner differs from native peer')
    return read_stages(text, proxy['childPid'], witness['threadId'], proxy['childCreation'], readback=readback)


def run_diagnostic(old, case, host, out, receipt, frozen):
    case_dir = out/'clear-soak'; case_dir.mkdir()
    def copy_binary(source, target):
        before = identity(source); require(not target.exists(), 'fresh diagnostic binary only')
        shutil.copy2(source, target); after = identity(target)
        require(all(before[k] == after[k] for k in ('bytes', 'sha256')), 'diagnostic copy differs')
        receipt['copies'].append(dict(source=before, copy=after)); frozen.extend((before, after))
        return target
    argv = case['process']['argv']
    fixture = copy_binary(Path(argv[0]), case_dir/'d3d9-presenter-fault32.exe')
    proxy = copy_binary(Path(argv[2]), case_dir/'proxy.dll')
    local_host = copy_binary(host, case_dir/'warvk-d3d9-host-dev.exe')
    binaries = {Path(r['path']).name: Path(r['path']) for r in old['binaries']}
    probes = {bits: copy_binary(binaries[f'desktop-probe-{bits}.exe'], out/f'desktop-probe-{bits}.exe') for bits in (32, 64)}
    env = recipe_environment(dict(os.environ), case_dir, local_host)
    receipt['actualSafeEnvironment'] = env
    require(not (case_dir/'absent.conf').exists(), 'diagnostic config must stay absent')
    with IsolatedRenderDesktop(restricted_medium=True) as desktop:
        receipt['desktopSafety'] = desktop.receipt
        probe_env = {k: v for k, v in env.items() if k in ENV_ALLOW or k in ('DXVK_LOG_LEVEL', 'DXVK_SHADER_CACHE')}
        try:
            receipt['probe'] = desktop.run([probes[32], '--expect-desktop', desktop.desktop_name,
                '--expect-station', 'WinSta0', '--input-desktop', desktop.input_before,
                '--child', probes[64], '--child-log', out/'probe-child.log'], cwd=out,
                stdout_path=out/'probe.stdout', stderr_path=out/'probe.stderr', timeout_s=30, env=probe_env)
        finally:
            receipt['probeRawArtifacts'] = capture_raw_artifacts(out, ['probe.stdout', 'probe.stderr', 'probe-child.log'])
        check_launch(receipt['probe'], desktop.desktop_name, desktop.input_before)
        rows = json_lines(out/'probe.stdout')+json_lines(out/'probe-child.log')
        require(len(rows) == 2, 'two actual bitness probes required')
        validate_probe_receipt(rows[0], desktop=desktop.desktop_name, input_desktop=desktop.input_before,
            station='WinSta0', bits=32, child=rows[1], launch=receipt['probe'])
        arguments = replay_argv(argv, fixture, proxy, desktop.desktop_name, desktop.input_before)
        row = dict(label='clear-soak', acceptancePassed=False); receipt['cases'].append(row)
        try:
            row['process'] = desktop.run(arguments, cwd=case_dir, stdout_path=case_dir/'case.stdout',
                stderr_path=case_dir/'case.stderr', timeout_s=case['process']['timeoutSeconds'], env=env)
        finally:
            # Even a launch exception retains all available raw bytes/missing names.
            row['initialRawArtifacts'] = capture_raw_artifacts(case_dir, RAW_NAMES, True)
        check_diagnostic_settlement(row['process'], desktop.desktop_name, desktop.input_before)
        hosts = json_lines(case_dir/'host.log'); reports = json_lines(case_dir/'case.stdout')
        row['stages'] = read_bound_stages((case_dir/'host.log').read_text('utf-8'), hosts, reports,
            row['process'], desktop.desktop_name, desktop.input_before)
        row['childSettlement'] = child_settled(row['stages']['pid'], row['stages']['creation100ns'])
        row['fixtureExitCode'] = row['process']['exitCode']
    safety = receipt['desktopSafety']
    require(safety['desktopClosed'] is True and safety['cleanupErrors'] == []
            and safety['inputAfterClose'] == safety['inputBefore'], 'diagnostic Desktop not cleaned')
    # safety.ok can be false because the fixture itself failed: do not rewrite it.
    receipt['diagnosticCollected'] = True


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--execute-native', action='store_true', help='root build/native lease required')
    args = ap.parse_args(); out = args.output.resolve()
    require(out.is_relative_to(ROOT/'AutoTest/artifacts') and out != ROOT/'AutoTest/artifacts', 'fresh artifact subdirectory')
    out.mkdir(parents=False, exist_ok=False)
    receipt = dict(schema=1, scope='R10_INITIALIZATION_COPY_TU_DIAGNOSTIC_ONLY', acceptancePassed=False,
        prepared=False, diagnosticCollected=False, nativeRequested=args.execute_native, rootBuildNativeLeaseRequired=True,
        historicalEnvironmentUnrecorded=True, historicalEnvironmentExactlyReproduced=False,
        recipeReproducedNotSameEnvironment=True, loggingCanPerturbTiming=True, driverColdStartProven=False,
        productionInputsWritten=False, gameTouched=False, playerDllChanged=None, systemChanged=False,
        productIntegrated=False, memorySavingsProven=False, originalTransportMilliseconds=5000,
        outerFixtureSeconds=45, priority='BelowNormal', parallelism=1, maximumParallelism=2,
        steps=[], stepRawArtifacts=[], copies=[], cases=[], frozenInputsPreserved=False, rawArtifactsPreserved=False)
    frozen = []; own = []
    try:
        old, case, build, original_rows = bind_baseline(); frozen.extend(original_rows)
        receipt['baselineReceipt'] = identity(BASELINE); receipt['originalHostSha256'] = HOST_SHA
        receipt['originalCaseArgv'] = case['process']['argv']; receipt['environmentRecipeSource'] = identity(RECIPE)
        # Freeze helpers actually used now; don't pretend changed Python readers
        # are historical binaries. Moving the two draft scripts remains supported.
        helpers = {Path(__file__).resolve(), Path(__file__).with_name('test_render_host_initialization.py').resolve()}
        helpers.update(Path(module.__file__).resolve() for module in list(sys.modules.values())
            if getattr(module, '__file__', None) and Path(module.__file__).suffix == '.py'
            and Path(module.__file__).resolve().is_relative_to(ROOT/'AutoTest'))
        frozen.extend(identity(p) for p in helpers)
        original = BACKEND.read_bytes(); require(b'\r' not in original.replace(b'\r\n', b''), 'mixed source newlines')
        normalized = original.decode('utf-8').replace('\r\n', '\n')
        changed, edits = instrument_source(normalized)
        directory = out/'substituted'; directory.mkdir(); source = directory/'d3d9_backend.cpp'
        with source.open('xb') as stream:
            stream.write(changed.replace('\n', '\r\n').encode('utf-8') if b'\r\n' in original else changed.encode('utf-8'))
        delta = ''.join(difflib.unified_diff(normalized.splitlines(True), changed.splitlines(True),
            fromfile=str(BACKEND), tofile=str(source)))
        with (out/'instrumentation.diff').open('x', encoding='utf-8', newline='') as stream:
            stream.write(delta)
        receipt.update(originalSource=identity(BACKEND), diagnosticSource=identity(source),
            instrumentationEdits=edits, instrumentationDiff=identity(out/'instrumentation.diff'), prepared=True)
        own.extend((receipt['diagnosticSource'], receipt['instrumentationDiff']))
        if args.execute_native:
            require(os.name == 'nt', 'Windows diagnostic only')
            env = recipe_environment(dict(os.environ), out/'clear-soak', out/'warvk-d3d9-host-dev.exe')
            receipt['buildSafeEnvironment'] = env
            host = build_diagnostic(old, build, source, out, env, receipt, frozen)
            own.extend((receipt['diagnosticObject'], receipt['diagnosticHost']))
            run_diagnostic(old, case, host, out, receipt, frozen)
    except Exception as error:
        receipt['error'] = str(error)
    finally:
        receipt['frozenInputs'] = frozen
        try:
            for row in frozen+own:
                copied.same_file(row)
            receipt['frozenInputsPreserved'] = bool(frozen)
            if 'old' in locals():
                receipt['playerAfter'] = identity(Path(old['playerBefore']['path']))
                receipt['playerDllChanged'] = receipt['playerAfter'] != old['playerBefore']
                require(receipt['playerDllChanged'] is False, 'player DLL changed')
        except Exception as error:
            receipt['preservationError'] = str(error)
        try:
            manifests = list(raw_manifests(receipt))
            for manifest in manifests:
                verify_raw_artifacts(manifest)
            receipt['rawArtifactsPreserved'] = bool(manifests)
        except Exception as error:
            receipt['rawPreservationError'] = str(error)
        receipt['diagnosticComplete'] = bool(receipt['diagnosticCollected'] and receipt['frozenInputsPreserved']
            and receipt['rawArtifactsPreserved'] and not any(k in receipt for k in
                ('error', 'preservationError', 'rawPreservationError')))
        receipt['preparationComplete'] = bool(receipt['prepared'] and receipt['frozenInputsPreserved']
            and not any(k in receipt for k in ('error', 'preservationError', 'rawPreservationError')))
        receipt['acceptancePassed'] = False
        with (out/'receipt.json').open('x', encoding='utf-8') as stream:
            json.dump(receipt, stream, ensure_ascii=False, indent=2)
    print(json.dumps({k: receipt.get(k) for k in ('acceptancePassed', 'prepared', 'diagnosticComplete', 'error',
        'preservationError', 'rawPreservationError')}, ensure_ascii=False))
    return 0 if (receipt['diagnosticComplete'] if args.execute_native else receipt['preparationComplete']) else 1


if __name__ == '__main__':
    sys.exit(main())
