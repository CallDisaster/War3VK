"""Pure mutation-reader/argv tests. No compiler, native launch or GPU calls."""
import copy
import json
from pathlib import Path
import struct
import subprocess
import unittest
from unittest.mock import patch

from isolated_render_desktop import DesktopSafetyError
import run_render_host_memory_destroy_mutation as gate
import test_render_host_memory_gate as fixture
from test_isolated_render_desktop_contract import sample as probe_sample


def negative():
    parents, hosts, launch = fixture.records()
    original = copy.deepcopy((parents, hosts))
    proxy = copy.deepcopy(parents[-2]); proxy['exchanges'] = 26
    reports = copy.deepcopy(parents[:3])+[proxy, dict(schema=gate.ERROR, ok=False, error=gate.EXPECTED_ERROR)]
    observations = copy.deepcopy(hosts[:5])
    raw = bytearray.fromhex(observations[-1]['wireHex'])
    struct.pack_into('<I', raw, 216, 0)
    struct.pack_into('<Q', raw, 248, 12)
    observations[-1]['wireHex'] = raw.hex()
    final = copy.deepcopy(hosts[-1]); final.update(gate.HOST_COUNTS); observations.append(final)
    launch.update(ok=False, exitCode=1)
    return reports, observations, launch, fixture.DESKTOP, fixture.INPUT, *original


def completed(restricted=False):
    state = negative(); reports, hosts, launch, desktop, initial, parents, baseline_hosts = state
    probe = copy.deepcopy(launch)
    probe.update(ok=True, exitCode=0, pid=501, creation100ns=50001, initialThreadId=601,
                 argv=['desktop-probe-32.exe'], actualExecutable='desktop-probe-32.exe')
    rows = [probe_sample(32, 501, 601), probe_sample(64, 502, 602)]
    rows[0]['creation100ns'] = 50001
    rows[0].update(childRequested=True, childDesktopExplicit=True,
                   childPid=rows[1]['pid'], childCreation=rows[1]['creation100ns'])
    for row in rows:
        for role in ('main', 'pump'): row[role]['desktop'] = desktop
    raw = dict(schema=1, scope='NON_INPUT_DESKTOP_LAUNCH_CONTEXT', ok=False, desktop=desktop,
        desktopCreated=True, desktopClosed=True, creatorExited=True, desktopHandleInheritable=False,
        desktopSwitchRequested=False, restrictedMediumRequested=restricted, globalInputUsed=False,
        gameTouched=False, launches=[probe, launch], cleanupErrors=[], windowStation='WinSta0',
        launcherDesktop=initial, desktopPath='WinSta0\\'+desktop, inputBefore=initial,
        inputAfterCreate=initial, inputBeforeLaunch=initial, inputAfterLaunch=initial,
        inputAfterRun=initial, inputAfterClose=initial)
    if restricted:
        for row in (probe, launch):
            row['restrictedToken'] = dict(schema=1, mode='restricted-medium-per-child/v1', ready=True,
                parentTokenUnchanged=True, derivedTokenClosed=True, childTokenVerifiedBeforeResume=True,
                noHighIntegrityFallback=True, resumedAfterVerification=True, resumePreviousCount=1,
                globalSecurityChanged=False, cleanupErrors=[])
    receipt = dict(desktopSafety=raw, desktopName=desktop, restrictedMediumRequested=restricted,
        probe=probe, probeReports=rows, process=launch, reports=reports, hostReports=hosts,
        mutation=gate.check_negative(*state), childSettlement='exact-process-handle-signaled')
    return receipt, parents, baseline_hosts


def baseline_fixture():
    parents, hosts, launch = fixture.records()
    def file(name):
        return dict(path=str(gate.ROOT/'synthetic-reader-only'/name), bytes=1, sha256='0'*64)
    source = file('source.cpp')
    baseline = dict(schema=1, scope='M0_ACTUAL_X86_X64_C2_MEMORY_ACCOUNT', ok=True,
        sourcesPreserved=True, sourceAndReferencePreserved=True, playerDllChanged=False,
        cases=[dict(ok=True, process=launch, artifacts=[file('case.stdout'), file('host.log')])],
        sources=[source], binaries=[file('fixture.exe')], compilers=[file('compiler.exe')],
        compileCommands=file('compile_commands.json'), referenceBefore=file('reference.dll'),
        referenceAfter=file('reference.dll'), playerBefore=file('player.dll'), playerAfter=file('player.dll'),
        caseCopies=[dict(source=file('fixture.exe'), copy=file('fixture-copy.exe'))],
        desktopName=fixture.DESKTOP, desktopSafety=dict(inputBefore=fixture.INPUT,
        restrictedMediumRequested=False), restrictedMediumRequested=False)
    return baseline, parents, hosts


class DestroyMutationTests(unittest.TestCase):
    def reject(self, state):
        with self.assertRaises((ValueError, KeyError, TypeError)):
            gate.check_negative(*state)

    def test_expected_specific_negative(self):
        result = gate.check_negative(*negative())
        self.assertTrue(result['killed'])
        self.assertEqual(result['firstRefusedPhase'], 4)
        self.assertEqual(result['firstRefusedOrdinal'], 1)
        self.assertEqual(result['firstRefusedSequence'], 24)
        self.assertEqual(result['childExit'], 0)
        self.assertFalse(result['genericFailureAccepted'])
        self.assertFalse(result['baselineRewritten'])

    def test_unrelated_failure_or_crash_not_killed(self):
        for error in ('unexpected error', 'actual memory checkpoint: HRESULT 0x88760868', '', None):
            state = negative(); state[0][-1]['error'] = error
            with self.subTest(error=error): self.reject(state)
        for field, bad in (('exitCode', 0), ('exitCode', 2), ('exitCode', 0xc0000005), ('exitCode', True),
                           ('ok', True), ('timedOut', True), ('forcedTermination', True),
                           ('jobEmpty', False), ('processSettled', False), ('jobActiveProcesses', 1),
                           ('actualExecutable', 'wrong.exe'), ('error', 'late cleanup failure')):
            state = negative(); state[2][field] = bad
            with self.subTest(field=field, bad=bad): self.reject(state)

    def test_child_failure_or_late_secondary_exit_not_killed(self):
        for field, bad in (('childExit', 2), ('transportFailed', True), ('childNaturalExit', False),
                           ('childProcessSettled', False), ('childSelfFailStop', True), ('primaryExitWaitMs', 5001),
                           ('exchanges', 25)):
            state = negative(); state[0][-2][field] = bad
            with self.subTest(field=field): self.reject(state)

    def test_host_workload_or_cleanup_mismatch(self):
        for field in ('accepted', 'backendCalls', 'submitted', 'retired', 'draws', 'logicalHandlesDestroyed', 'liveResources'):
            state = negative(); state[1][-1][field] += 1
            with self.subTest(field=field): self.reject(state)
        state = negative(); state[1][-1]['normalTeardown'] = False; self.reject(state)

    def test_prefix_must_really_pass(self):
        for index in range(3):
            for name in ('pid', 'thread', 'creation', 'expectedHostCreation', 'expectedSequence', 'logicalHandles'):
                state = negative(); state[0][index]['parent'][name] += 1
                with self.subTest(index=index, field=name): self.reject(state)
        state = negative(); state[0][1]['parent']['cpu']['kinds']['indexBuffer']['cpuCapacityBytes'] += 1; self.reject(state)
        state = negative(); state[0][2]['parent']['beforeEnd'] = 999999; self.reject(state)

    def test_host_bad_sample_exact_shape(self):
        for offset, fmt, value in ((216, 'I', 1), (216, 'I', 2), (248, 'Q', 0), (248, 'Q', 13),
            (240, 'Q', 12), (224, 'Q', 1), (232, 'Q', 12), (280, 'Q', 12), (220, 'I', 1),
            (480, 'I', 1), (511, 'B', 1), (8, 'I', 3), (12, 'I', 2), (56, 'Q', 23),
            (16, 'I', 203), (32, 'Q', 9), (40, 'Q', 7), (48, 'Q', 7), (152, 'I', 0), (156, 'I', 5)):
            state = negative(); raw = bytearray.fromhex(state[1][4]['wireHex'])
            struct.pack_into('<'+fmt, raw, offset, value); state[1][4]['wireHex'] = raw.hex()
            with self.subTest(offset=offset, value=value): self.reject(state)

    def test_record_population_order_and_exact_schema(self):
        for stream in (0, 1):
            for index in range(len(negative()[stream])):
                state = negative(); del state[stream][index]
                with self.subTest(stream=stream, index=index): self.reject(state)
            state = negative(); state[stream].append(copy.deepcopy(state[stream][-1])); self.reject(state)
            state = negative(); state[stream][0], state[stream][1] = state[stream][1], state[stream][0]; self.reject(state)
        state = negative(); state[1][0]['extra'] = True; self.reject(state)
        state = negative(); state[0][-1]['extra'] = True; self.reject(state)

    def test_negative_decoder_is_not_normalized(self):
        state = negative(); wire = state[1][4]['wireHex']; before = copy.deepcopy(state)
        observed = gate.decode_dead_host(wire)
        self.assertFalse(observed['cpu']['valid'])
        self.assertEqual(observed['cpu']['kinds']['vertexBuffer']['cpuCapacityBytes'], 12)
        self.assertEqual(state, before)
        with self.assertRaises(ValueError): gate.memory.decode_host(wire)

    def test_complete_expected_negative_cleanup_preserves_raw_false(self):
        for restricted in (False, True):
            state = completed(restricted); before = copy.deepcopy(state)
            result = gate.check_mutation_desktop_cleanup(*state)
            self.assertTrue(result['accepted'])
            self.assertFalse(result['rawDesktopOk'])
            self.assertFalse(result['rawNegativeLaunchOk'])
            self.assertFalse(result['rawEvidenceRewritten'])
            self.assertFalse(result['independentHandleCensusMeasured'])
            self.assertEqual(state, before)
            self.assertFalse(state[0]['desktopSafety']['ok'])
            self.assertFalse(state[0]['process']['ok'])
            self.assertEqual(state[0]['process']['exitCode'], 1)

    def test_complete_cleanup_fails_closed(self):
        mutations = (('ok', True), ('desktopClosed', False), ('desktopCreated', False),
            ('creatorExited', False), ('desktopHandleInheritable', True), ('desktopSwitchRequested', True),
            ('globalInputUsed', True), ('gameTouched', True), ('cleanupErrors', ['CloseHandle:6']),
            ('bodyError', 'unexpected'), ('windowStation', 'Other'), ('desktopPath', 'WinSta0\\Default'),
            ('inputAfterCreate', 'Changed'), ('inputBeforeLaunch', 'Changed'), ('inputAfterLaunch', 'Changed'),
            ('inputAfterRun', 'Changed'), ('inputAfterClose', 'Changed'), ('restrictedMediumRequested', True))
        for field, value in mutations:
            state = completed(); state[0]['desktopSafety'][field] = value
            with self.subTest(field=field), self.assertRaises((ValueError, KeyError, TypeError)):
                gate.check_mutation_desktop_cleanup(*state)
        for role in ('probe', 'process'):
            for field, value in (('jobActiveProcesses', 1), ('jobEmpty', False), ('timedOut', True),
                ('forcedTermination', True), ('processSettled', False), ('cleanupErrors', ['CloseHandle:6']),
                ('inputAfter', 'Changed'), ('error', 'not successful')):
                state = completed(); state[0][role][field] = value
                with self.subTest(role=role, field=field), self.assertRaises((ValueError, KeyError, TypeError)):
                    gate.check_mutation_desktop_cleanup(*state)

    def test_complete_requires_exact_two_launches_and_raw_certification(self):
        for mutation in ('extra', 'missing', 'swap', 'replace', 'probe-record', 'negative-record',
                         'stored-verdict', 'child-settlement', 'raw-ok'):
            state = completed(); row = state[0]; launches = row['desktopSafety']['launches']
            if mutation == 'extra': launches.append(copy.deepcopy(row['probe']))
            elif mutation == 'missing': launches.pop()
            elif mutation == 'swap': launches.reverse()
            elif mutation == 'replace': launches[1] = copy.deepcopy(row['probe'])
            elif mutation == 'probe-record': row['probeReports'][0]['pid'] += 1
            elif mutation == 'negative-record': row['reports'][-1]['error'] = 'unrelated failure'
            elif mutation == 'stored-verdict': row['mutation']['killed'] = False
            elif mutation == 'child-settlement': row['childSettlement'] = 'unknown'
            elif mutation == 'raw-ok': row['process']['ok'] = True
            with self.subTest(mutation=mutation), self.assertRaises((ValueError, KeyError, TypeError, DesktopSafetyError)):
                gate.check_mutation_desktop_cleanup(*state)

    def test_complete_restricted_token_cleanup_required(self):
        for role in ('probe', 'process'):
            for key, value in (('parentTokenUnchanged', False), ('derivedTokenClosed', False),
                ('childTokenVerifiedBeforeResume', False), ('noHighIntegrityFallback', False),
                ('resumedAfterVerification', False), ('resumePreviousCount', 2),
                ('globalSecurityChanged', True), ('cleanupErrors', ['token-close:6'])):
                state = completed(True); state[0][role]['restrictedToken'][key] = value
                with self.subTest(role=role, key=key), self.assertRaises((ValueError, KeyError, TypeError)):
                    gate.check_mutation_desktop_cleanup(*state)

    def test_baseline_process_policy_cannot_change_in_mutation(self):
        for expected in (False, True):
            baseline = dict(restrictedMediumRequested=expected,
                            desktopSafety=dict(restrictedMediumRequested=expected))
            gate.check_baseline_process_policy(baseline, expected)
            with self.assertRaises(ValueError): gate.check_baseline_process_policy(baseline, not expected)
            baseline['desktopSafety']['restrictedMediumRequested'] = not expected
            with self.assertRaises(ValueError): gate.check_baseline_process_policy(baseline, expected)

    def baseline_check(self, state, changed_file=False):
        baseline, parents, hosts = state
        # Synthetic metadata is isolated from real file/native I/O. The actual
        # production checkpoint reader still runs; this is not freeze evidence.
        with patch.object(Path, 'read_text', return_value=json.dumps(baseline)), \
             patch.object(gate.memory, 'source_set', return_value=[gate.ROOT/'synthetic-reader-only/source.cpp']), \
             patch.object(gate, 'same_file', side_effect=ValueError('file changed') if changed_file else lambda row: row), \
             patch.object(gate, 'json_lines', side_effect=[parents, hosts]):
            return gate.verify_baseline(Path('synthetic-baseline.json'))

    def test_baseline_replays_strict_native_reader(self):
        state = baseline_fixture(); result = self.baseline_check(state)
        self.assertEqual(result, state)
        with self.assertRaises(ValueError): self.baseline_check(state, changed_file=True)

    def test_old_failed_incomplete_or_forged_baseline_refused(self):
        for mutation in ('old-helper', 'failed', 'case-failed', 'missing', 'fake-source', 'raw-error', 'copy-changed'):
            state = baseline_fixture(); baseline, parents, hosts = state
            if mutation == 'old-helper': parents[-1]['cpuHelperChecks'] = 30
            elif mutation == 'failed': baseline['ok'] = False
            elif mutation == 'case-failed': baseline['cases'][0]['ok'] = False
            elif mutation == 'missing': del baseline['sourcesPreserved']
            elif mutation == 'fake-source': baseline['sources'][0]['path'] = str(gate.ROOT/'other.cpp')
            elif mutation == 'raw-error': parents[-1]['pixelFailures'] = 1
            elif mutation == 'copy-changed': baseline['caseCopies'][0]['copy']['sha256'] = '1'*64
            with self.subTest(mutation=mutation), self.assertRaises((ValueError, KeyError, TypeError)):
                self.baseline_check(state)

    def test_windows_command_argv_roundtrip(self):
        for argv in ([r'E:\tool path\clang++.exe', r'-IE:\source path\include', '-o', r'C:\out\a.obj', '-c', r'..\src\a.cpp'],
            ['clang++.exe', '', 'abc', 'ends\\', 'space here', 'literal"quote', '\\"'],
            ['clang++.exe', '-DNAME="value here"', r'\server\share\a.cpp', '-Wl,--start-group']):
            with self.subTest(argv=argv): self.assertEqual(gate.windows_argv(subprocess.list2cmdline(argv)), argv)

    def test_shell_or_response_commands_refused(self):
        for command in ('', 'tool @args.rsp', 'tool && next', 'tool | next', 'tool > out', '"unclosed', 'tool\nnext'):
            with self.subTest(command=command), self.assertRaises(ValueError): gate.windows_argv(command)

    def test_source_mutation_exact_once_and_location(self):
        original = 'void D3D9Backend::destroy(int i) {\n        slot = Owner::Buffer{};\n}\n'
        changed = gate.mutate_source(original)
        self.assertEqual(changed.count('slot = Owner::Buffer{};'), 1)
        self.assertIn('slot.indexBytes = std::move(retainedIndexBytes);', changed)
        self.assertIn('slot.indexBytes.clear();', changed)
        for bad in ('no destroy', original+original, original.replace('slot = ', 'slot=')):
            with self.subTest(bad=bad), self.assertRaises(ValueError): gate.mutate_source(bad)

    def test_compile_only_permitted_tokens_change(self):
        cwd = gate.ROOT/'build-pure-placeholder'; original = cwd/'host.p/backend.obj'
        argv = ['compiler.exe', '-O3', '-DVALUE=a b', '-I../src', '-MD', '-MQ', str(original),
            '-MF', str(original)+'.d', '-o', str(original), '-c', str(gate.BACKEND)]
        entry = dict(directory=str(cwd), arguments=argv, file=str(gate.BACKEND), output=str(original))
        plan = gate.compile_plan(entry, cwd/'copied.cpp', cwd/'new.obj', cwd/'new.d')
        changed = set(plan['substitutions'].values())
        self.assertEqual([x for i, x in enumerate(argv) if i not in changed],
                         [x for i, x in enumerate(plan['argv'][:-2]) if i not in changed])
        self.assertEqual(plan['argv'][-2:], ['-iquote', str(gate.BACKEND.parent)])
        self.assertEqual(entry['arguments'], argv)
        bad = copy.deepcopy(entry); bad['arguments'][6] = str(cwd/'other.obj')
        with self.assertRaises(ValueError): gate.compile_plan(bad, cwd/'copy.cpp', cwd/'new.obj', cwd/'new.d')

    def test_link_only_backend_and_output_change(self):
        cwd = gate.ROOT/'build-pure-placeholder'; original = cwd/'host.p/backend.obj'
        argv = ['compiler.exe', '-o', str(cwd/gate.TARGET), str(cwd/'main.obj'), str(original),
                '-Wl,--start-group', str(cwd/'core.a'), '-lgdi32', '-Wl,--end-group']
        command = subprocess.list2cmdline(argv)
        plan = gate.link_plan(command, cwd, original, cwd/'new.obj', cwd/'new-host.exe')
        for i, value in enumerate(argv):
            if i not in plan['substitutions'].values(): self.assertEqual(plan['argv'][i], value)
        for bad in (command+'\n'+command, command.replace(str(original), str(cwd/'other.obj'))):
            with self.assertRaises(ValueError): gate.link_plan(bad, cwd, original, cwd/'new.obj', cwd/'new-host.exe')

    def test_declared_libraries_bound(self):
        cwd = gate.ROOT/'build-pure-placeholder'
        query = 'host.exe:\n  input: cpp_LINKER\n    main.obj\n    | core.a\n    | libgdi32.a\n  outputs:\n'
        link = dict(originalArgv=['compiler.exe', '-o', 'host.exe', 'main.obj', 'core.a', '-lgdi32'])
        result = gate.declared_link_inputs(query, cwd, link)
        self.assertEqual(set(result), {cwd/'main.obj', cwd/'core.a', cwd/'libgdi32.a'})
        with self.assertRaises(ValueError): gate.declared_link_inputs(query.replace('    | libgdi32.a\n', ''), cwd, link)

    def test_thin_archive_external_members_match_actual_archive(self):
        cwd = gate.ROOT/'build-pure-placeholder'; archive = cwd/'src/core.a'
        query = 'src/core.a:\n  input: STATIC_LINKER\n    src/core.a.p/a.obj\n    src/core.a.p/b.obj\n    | ../core.map\n  outputs:\n    host.exe\n'
        listing = 'src/core.a.p/a.obj\nsrc/core.a.p/b.obj\n'
        self.assertEqual(gate.thin_archive_members(query, listing, cwd, archive),
                         [cwd/'src/core.a.p/a.obj', cwd/'src/core.a.p/b.obj'])
        for bad in ('', listing+'src/core.a.p/c.obj\n', listing+listing,
                    'src/core.a.p/a.obj\n', 'src/core.a.p/a.obj\n../foreign.obj\n'):
            with self.subTest(listing=bad), self.assertRaises(ValueError):
                gate.thin_archive_members(query, bad, cwd, archive)
        for bad in (query.replace('src/core.a:', 'other.a:'), query.replace('STATIC_LINKER', 'cpp_LINKER'),
                    query.replace('    src/core.a.p/b.obj\n', ''), query.replace('    src/core.a.p/b.obj\n', '    src/core.a.p/a.obj\n')):
            with self.subTest(query=bad), self.assertRaises(ValueError):
                gate.thin_archive_members(bad, listing, cwd, archive)
        for name in ('../foreign.obj', 'src/nested.a', 'src/header.h'):
            bad_query = 'src/core.a:\n  input: STATIC_LINKER\n    '+name+'\n  outputs:\n'
            with self.subTest(name=name), self.assertRaises(ValueError):
                gate.thin_archive_members(bad_query, name+'\n', cwd, archive)


if __name__ == '__main__':
    unittest.main(verbosity=2)
