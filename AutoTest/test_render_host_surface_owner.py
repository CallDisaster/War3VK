"""Pure transformation/reader tests. These do not execute Win32 or Vulkan."""
import copy
import json
import unittest
from pathlib import Path

import diagnose_render_host_surface_owner as m


def records(mode='host'):
    launch = dict(pid=10, desktop='Isolated-test', inputBefore='Default')
    witness = dict(pid=20, peerPid=10, threadId=21, windowThreadId=11,
                   desktop='Isolated-test', inputDesktop='Default')
    proxy = dict(childPid=20, childCreation=100)
    marker = 'WarVK-D9C1-'+'a'*32
    rows = []
    for i, event in enumerate(m.EVENTS):
        rows.append(dict(event=event, sequence=i+1, mode=mode, pid=20, creation100ns=100,
            mainTid=21, peerHwnd=101, peerPid=10, peerTid=11, peerMarker=marker,
            ownedHwnd=201, ownedTid=22, ownedThreadCreation100ns=101, ownedMarker=marker+'-diagnostic-owned',
            surfaceHwnd=201 if mode == 'host' else 101, surfacePid=20 if mode == 'host' else 10,
            surfaceTid=22 if mode == 'host' else 11, surfaceHinstance=0 if not i else 800,
            desktop='Isolated-test', inputDesktop='Default', surfaceCalls=int(i != 0),
            pulses=1 if i < 2 else 2, messages=i+1, destroyed=int(i == 3), ncDestroyed=int(i == 3),
            classUnregistered=i == 3, failure=0, closeMs=3 if i == 3 else 0))
    return rows, launch, witness, proxy


def text(rows):
    return '\n'.join(m.PREFIX+json.dumps(r) for r in rows)


class ReaderTests(unittest.TestCase):
    def read(self, rows, mode='host', launch=None, witness=None, proxy=None):
        _, a, b, c = records(mode)
        return m.read_target(text(rows), mode, launch or a, witness or b, proxy or c)

    def test_two_arms_are_distinct_and_never_acceptance(self):
        for mode in ('peer', 'host'):
            result = self.read(records(mode)[0], mode)
            self.assertTrue(result['targetLifecycleComplete'])
            self.assertFalse(result['acceptancePassed'])
            self.assertEqual(result['peerWindowPresented'], mode == 'peer')

    def test_all_native_fields_reject_bool_negative_fraction_string(self):
        rows = records()[0]
        fields = [k for k, v in rows[0].items() if type(v) is int]
        for i in range(4):
            for name in fields:
                for value in (True, -1, 0.5, '1', 2**64):
                    with self.subTest(index=i, field=name, value=value):
                        changed = copy.deepcopy(rows); changed[i][name] = value
                        with self.assertRaises(Exception): self.read(changed)

    def test_missing_duplicate_order(self):
        rows = records()[0]
        for changed in ([], rows[:3], rows+[rows[-1]], [rows[1], rows[0], *rows[2:]]):
            with self.assertRaises(Exception): self.read(changed)

    def test_each_identity_field_drift_is_rejected(self):
        rows = records()[0]
        for field in ('pid', 'creation100ns', 'mainTid', 'peerHwnd', 'peerPid', 'peerTid',
                      'ownedHwnd', 'ownedTid', 'ownedThreadCreation100ns', 'surfaceHwnd', 'surfacePid', 'surfaceTid'):
            changed = copy.deepcopy(rows); changed[2][field] += 1
            with self.subTest(field=field), self.assertRaises(Exception): self.read(changed)

    def test_wrong_mode_marker_desktop_and_close(self):
        for field, value in (('mode', 'peer'), ('peerMarker', 'not-a-session'), ('ownedMarker', 'reused'),
                             ('desktop', 'Default'), ('inputDesktop', 'Isolated-test')):
            changed = records()[0]
            for row in changed: row[field] = value
            with self.subTest(field=field), self.assertRaises(Exception): self.read(changed)
        for field, value in (('failure', 1), ('closeMs', 1000), ('destroyed', 0), ('ncDestroyed', 0),
                             ('classUnregistered', 1), ('pulses', 1), ('surfaceCalls', 2), ('surfaceHinstance', 0)):
            changed = records()[0]; changed[-1][field] = value
            with self.subTest(field=field), self.assertRaises(Exception): self.read(changed)

    def test_entire_coherent_wrong_target_rejected(self):
        changed = records()[0]
        for row in changed:
            row.update(surfaceHwnd=row['peerHwnd'], surfacePid=row['peerPid'], surfaceTid=row['peerTid'])
        with self.assertRaises(Exception): self.read(changed)

    def test_coherent_out_of_range_dword_not_upgraded_to_uint64(self):
        for mode, keys in (('host', ('ownedTid', 'surfaceTid')), ('peer', ('peerTid', 'surfaceTid'))):
            changed, launch, witness, proxy = records(mode)
            for row in changed:
                for key in keys: row[key] = 2**32+7
            if mode == 'peer': witness['windowThreadId'] = 2**32+7
            with self.assertRaises(Exception): self.read(changed, mode, launch, witness, proxy)

    def test_proxy_and_monitor_must_certify_same_creation(self):
        _, _, witness, proxy = records()
        binding = dict(host=dict(pid=20, creation100ns=100), nativeDesktopWitness=witness)
        m.check_monitor_proxy(binding, proxy, witness)
        for key in ('pid', 'creation100ns'):
            changed = copy.deepcopy(binding); changed['host'][key] += 1
            with self.assertRaises(Exception): m.check_monitor_proxy(changed, proxy, witness)

    def test_actual_witness_binding(self):
        rows, launch, witness, proxy = records()
        for source, key in ((launch, 'pid'), (witness, 'pid'), (witness, 'threadId'),
                            (witness, 'windowThreadId'), (proxy, 'childCreation')):
            original = source[key]; source[key] += 1
            with self.assertRaises(Exception): self.read(rows, launch=launch, witness=witness, proxy=proxy)
            source[key] = original

    def test_malformed_marker_and_duplicate_json_key(self):
        rows, a, b, c = records()
        valid = text(rows)
        for changed in (valid.replace(m.PREFIX, 'x'+m.PREFIX, 1),
                        valid.replace('"event": "ready"', '"event":"ready","event":"ready"', 1)):
            with self.assertRaises(Exception): m.read_target(changed, 'host', a, b, c)


class SourceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.original = m.init.BACKEND.read_text('utf-8')
        cls.changed, cls.edits, cls.timing = m.instrument_source(cls.original)

    def test_reversible_except_frozen_explicit_replacements(self):
        rebuilt = self.changed
        for edit in reversed(self.timing):
            anchor, insertion = edit['anchor'], edit['insertion']
            transformed = insertion+anchor if edit['side'] == 'before' else anchor+insertion
            self.assertEqual(rebuilt.count(transformed), 1)
            rebuilt = rebuilt.replace(transformed, anchor, 1)
        for edit in reversed(self.edits):
            self.assertEqual(rebuilt.count(edit['replacement']), 1)
            rebuilt = rebuilt.replace(edit['replacement'], edit['anchor'], 1)
        self.assertEqual(rebuilt, self.original)

    def test_original_peer_validator_unchanged(self):
        def validator(source):
            return source[source.index('  void validateWindow() const {'):source.index('\n  template<typename T, size_t N>')]
        self.assertEqual(validator(self.original), validator(self.changed))
        start = self.changed.index('void D3D9Backend::initialize(')
        init = self.changed[start:self.changed.index('void D3D9Backend::createBuffer(', start)]
        self.assertLess(init.index('owner.validateWindow();'), init.index('owner.diagnosticTarget.start('))
        self.assertIn('ownerPtr->validateWindow();', init)
        self.assertIn('wsi::createSurface(actualTarget,', init)
        self.assertIn('surfaceCreated(actualTarget, static_cast<int>(result))', init)

    def test_retire_close_device_order_and_existing_deadlines(self):
        start = self.changed.index('  void shutdown(Stats& stats) {')
        block = self.changed[start:self.changed.index('\n};', start)]
        markers = ('device->waitForIdle();', 'presenter->destroyResources();', 'presenter = nullptr;',
                   'diagnosticTarget.closeAfterSurfaceRetired();', 'device = nullptr;', 'instance = nullptr;')
        positions = [block.index(s) for s in markers]
        self.assertEqual(positions, sorted(positions))
        self.assertEqual(m.ARMS, ('peer', 'host', 'host', 'peer'))
        connection = (m.ROOT/'tools/render_host/d3d9_connection.h').read_text('utf-8')
        self.assertIn('childExit=child.wait(5000);', connection)
        self.assertIn('const lab::ReadWindow deadline;', connection)

    def test_header_only_explicit_diagnostic_owned_apis(self):
        header = m.HEADER.read_text('utf-8')
        for forbidden in ('TerminateThread(', 'SetThreadDesktop(', 'SwitchDesktop(', 'SendInput(',
                          'FreeLibrary(', 'wglDeleteContext(', 'WriteProcessMemory('):
            self.assertNotIn(forbidden, header)
        self.assertIn('GetEnvironmentVariableW(L"WARVK_DIAGNOSTIC_SURFACE_OWNER"', header)
        self.assertEqual(header.count('GetEnvironmentVariableW('), 1)
        self.assertIn('WaitForSingleObject(thread, 1000)', header)
        self.assertIn('WM_NCDESTROY', header)
        self.assertIn('GetThreadTimes(thread', header)
        self.assertIn('GetWindow(hwnd, GW_OWNER)', header)
        self.assertIn('GetWindowLongPtrW(actualTarget, GWLP_HINSTANCE)', header)
        for meson in (m.ROOT/'tools/render_host/meson.build', m.ROOT/'src/d3d9/meson.build'):
            self.assertNotIn(m.HEADER.name, meson.read_text('utf-8'))

    def test_source_drift_fails_closed(self):
        for edit in self.edits:
            changed = self.original.replace(edit['anchor'], edit['anchor']+'\n'+edit['anchor'], 1)
            with self.subTest(anchor=edit['anchor'][:40]), self.assertRaises(Exception):
                m.instrument_source(changed)


if __name__ == '__main__':
    unittest.main()
