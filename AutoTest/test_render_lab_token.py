"""Pure, injected token-owner/reader tests: no native token or process calls."""
import copy
import ctypes
from pathlib import Path
import struct
from types import SimpleNamespace
import unittest

import render_lab_token as token
import render_lab_default_dacl as dacl
from isolated_render_desktop import IsolatedRenderDesktop, DesktopSafetyError, _StartupInfoEx, _ProcessInfo


def snapshot(reduced=False):
    return dict(userSid='S-1-5-21-100-200-300-500', sessionId=1, tokenType=1,
        tokenId=222 if reduced else 111, authenticationId=333, modifiedId=444,
        integritySid='S-1-16-8192' if reduced else 'S-1-16-12288', integrityRid=8192 if reduced else 12288,
        groups=[dict(sid=token.ADMIN_SID, attributes=0x10 if reduced else 7),
                dict(sid='S-1-5-32-545', attributes=7),
                dict(sid='S-1-16-8192' if reduced else 'S-1-16-12288', attributes=0x60)],
        privileges=[dict(name='SeChangeNotifyPrivilege', attributes=3)] +
                   ([] if reduced else [dict(name='SeDebugPrivilege', attributes=2)]),
        elevated=True, uiAccess=False, appContainer=False, sandboxInert=False,
        hasRestrictions=reduced, handleInheritable=False)


def basic_acl():
    entries = []
    for sid, mask in ((token.ADMIN_SID, 0x10000000), ('S-1-5-18', 0x10000000),
                      ('S-1-5-5-0-1234', 0xA0000000)):
        data = dacl.encode_sid(sid)
        entries.append(struct.pack('<BBHI', 0, 0, 8 + len(data), mask) + data)
    return struct.pack('<BBHHH', 2, 0, 8 + sum(map(len, entries)), len(entries), 0) + b''.join(entries)


def pointed_buffer(payload):
    size = ctypes.sizeof(ctypes.c_void_p)
    buffer = ctypes.create_string_buffer(size + (0 if payload is None else len(payload)))
    if payload is not None:
        ctypes.c_void_p.from_buffer(buffer).value = ctypes.addressof(buffer) + size
        ctypes.memmove(ctypes.addressof(buffer) + size, payload, len(payload))
    return buffer


class FakeApi:
    def __init__(self):
        self.source = snapshot(); self.derived = snapshot(True); self.child = snapshot(True)
        self.source_count = 0; self.closed = []; self.operations = []
        self.fail = None; self.changed_parent = False
        self.source_default = dict(ownerSid=token.ADMIN_SID, primaryGroupSid='S-1-5-32-545', defaultDacl=basic_acl())
        self.derived_default = dict(self.source_default, ownerSid=self.derived['userSid'])
        self.child_default = copy.deepcopy(self.derived_default)
        self.default_writes = []
        self.a = SimpleNamespace(SetTokenInformation=self._set_token_information)

    def _op(self, name):
        self.operations.append(name)
        if self.fail == name: raise token.TokenSafetyError('injected-' + name)

    def open_current(self):
        self._op('open-current'); self.source_count += 1; return 1000+self.source_count

    def open_process(self, process):
        self._op('open-child'); token.require(process == 999, 'exact synthetic process handle'); return 3000

    def describe(self, handle):
        self._op('describe-' + ('derived' if handle == 2000 else 'child' if handle == 3000 else 'source'))
        return copy.deepcopy(self.derived if handle == 2000 else self.child if handle == 3000 else self.source)

    def restrict(self, source):
        self._op('restrict'); token.require(source == 1001, 'exact source token'); return 2000

    def set_medium(self, handle):
        self._op('set-medium'); token.require(handle == 2000, 'never adjust source token')

    def _query(self, handle, kind):
        self._op('query-' + ('derived' if handle == 2000 else 'child' if handle == 3000 else 'source') + '-' + str(kind))
        data = self.derived_default if handle == 2000 else self.child_default if handle == 3000 else self.source_default
        field = {4: 'ownerSid', 5: 'primaryGroupSid', 6: 'defaultDacl'}[kind]
        return pointed_buffer(data[field] if kind == 6 else dacl.encode_sid(data[field]))

    def _set_token_information(self, handle, kind, value, size):
        self._op('set-default-dacl')
        token.require(handle == 2000 and kind == 6 and size == ctypes.sizeof(ctypes.c_void_p), 'only private class6 write')
        pointer = ctypes.cast(value, ctypes.POINTER(ctypes.c_void_p)).contents.value
        length = ctypes.c_uint16.from_address(pointer + 2).value
        data = ctypes.string_at(pointer, length)
        self.default_writes.append((handle, kind, data))
        self.derived_default['defaultDacl'] = data
        self.derived['modifiedId'] += 1
        return True

    @staticmethod
    def check(value, operation):
        token.require(value, operation + ' fake failed')

    def close(self, handle):
        self._op('close-' + str(handle)); self.closed.append(handle)

    def create(self, handle, executable, command, flags, environment, directory, startup, info):
        self._op('create-restricted'); token.require(handle == 2000, 'derived token creation only')
        info.hProcess = 999; info.hThread = 998; info.dwProcessId = 997; info.dwThreadId = 996
        self.child_default = copy.deepcopy(self.derived_default)
        if self.changed_parent: self.source['integrityRid'] = 8192; self.source['integritySid'] = token.MEDIUM_SID


def startup():
    value = _StartupInfoEx(); value.StartupInfo.cb = ctypes.sizeof(value)
    value.StartupInfo.lpDesktop = 'WinSta0\\WarVK_ReaderOnly'
    value.StartupInfo.dwFlags = 0x100; value.StartupInfo.hStdInput = 11
    value.StartupInfo.hStdOutput = 12; value.StartupInfo.hStdError = 13; value.lpAttributeList = 14
    return value


def create(owner, value=None, flags=None):
    info = _ProcessInfo()
    owner.create_suspended('reader-only.exe', ctypes.create_unicode_buffer('reader-only.exe'),
        token.BASE_FLAGS | token.CREATE_SUSPENDED if flags is None else flags,
        None, 'reader-only-directory', startup() if value is None else value, info)
    return info


class TokenContractTests(unittest.TestCase):
    def test_construction_has_no_native_effect(self):
        owner = token.RestrictedMediumToken()
        self.assertIsNone(owner._api); self.assertEqual(owner.handle, 0)
        self.assertFalse(owner.receipt['ready'])
        desktop = IsolatedRenderDesktop(restricted_medium=True)
        self.assertIsNone(desktop._api); self.assertTrue(desktop.restricted_medium)
        self.assertFalse(IsolatedRenderDesktop().restricted_medium)
        for bad in (None, 1, 0, 'true'):
            with self.subTest(bad=bad), self.assertRaises(DesktopSafetyError): IsolatedRenderDesktop(restricted_medium=bad)

    def test_full_fake_handoff_and_parent_preserved(self):
        api = FakeApi(); owner = token.RestrictedMediumToken(_api=api)
        with owner:
            self.assertTrue(owner.receipt['ready']); info = create(owner)
            self.assertEqual(info.hProcess, 999)
            owner.verify_child(info.hProcess)
            self.assertTrue(owner.receipt['childTokenVerifiedBeforeResume'])
        self.assertTrue(owner.receipt['derivedTokenClosed'])
        self.assertTrue(owner.receipt['parentTokenUnchanged'])
        self.assertEqual(owner.receipt['cleanupErrors'], [])
        self.assertEqual(api.closed, [1001, 3000, 1002, 2000])
        self.assertEqual(api.operations.count('create-restricted'), 1)
        self.assertEqual(api.operations.count('set-medium'), 1)
        self.assertEqual(len(api.default_writes), 1)
        self.assertTrue(owner.receipt['defaultDacl']['ok'])
        self.assertEqual(owner.receipt['sourceDefaultBefore'], owner.receipt['sourceDefaultAfter'])
        self.assertEqual(owner.receipt['childDefaultBeforeResume']['defaultDacl'],
                         owner.receipt['defaultDacl']['derivedAfter']['defaultDacl'])

    def test_owner_single_use_and_one_child(self):
        api = FakeApi(); owner = token.RestrictedMediumToken(_api=api)
        with owner:
            create(owner)
            with self.assertRaises(token.TokenSafetyError): create(owner)
        with self.assertRaises(token.TokenSafetyError): owner.__enter__()
        with self.assertRaises(token.TokenSafetyError): create(owner)

    def test_all_creation_flags_exact(self):
        for flags in (token.BASE_FLAGS, token.BASE_FLAGS | token.CREATE_SUSPENDED | 0x10, 0, True):
            api = FakeApi()
            with token.RestrictedMediumToken(_api=api) as owner:
                with self.subTest(flags=flags), self.assertRaises(token.TokenSafetyError): create(owner, flags=flags)
            self.assertNotIn('create-restricted', api.operations)

    def test_no_missing_job_handle_list_or_default_desktop(self):
        for field, bad in (('lpAttributeList', None), ('cb', 0), ('lpDesktop', 'WinSta0\\Default'),
            ('lpDesktop', 'Default'), ('dwFlags', 0), ('hStdInput', None), ('hStdOutput', None), ('hStdError', None)):
            api = FakeApi(); value = startup()
            setattr(value if field == 'lpAttributeList' else value.StartupInfo, field, bad)
            with token.RestrictedMediumToken(_api=api) as owner:
                with self.subTest(field=field), self.assertRaises(token.TokenSafetyError): create(owner, value)
            self.assertNotIn('create-restricted', api.operations)

    def test_open_current_failure_never_changes_token(self):
        api = FakeApi(); api.fail = 'open-current'
        with self.assertRaisesRegex(token.TokenSafetyError, 'injected-open-current'):
            token.RestrictedMediumToken(_api=api).__enter__()
        self.assertNotIn('restrict', api.operations); self.assertNotIn('create-restricted', api.operations)

    def test_restrict_failure_closes_only_own_source(self):
        api = FakeApi(); api.fail = 'restrict'; owner = token.RestrictedMediumToken(_api=api)
        with self.assertRaisesRegex(token.TokenSafetyError, 'injected-restrict'): owner.__enter__()
        self.assertIn(1001, api.closed); self.assertNotIn(2000, api.closed)
        self.assertNotIn('create-restricted', api.operations)

    def test_medium_set_failure_closes_derived(self):
        api = FakeApi(); api.fail = 'set-medium'; owner = token.RestrictedMediumToken(_api=api)
        with self.assertRaisesRegex(token.TokenSafetyError, 'injected-set-medium'): owner.__enter__()
        self.assertIn(2000, api.closed); self.assertEqual(owner.handle, 0)

    def test_source_close_failure_cannot_leak_derived(self):
        api = FakeApi(); api.fail = 'close-1001'; owner = token.RestrictedMediumToken(_api=api)
        with self.assertRaisesRegex(token.TokenSafetyError, 'injected-close-1001'): owner.__enter__()
        self.assertIn(2000, api.closed); self.assertEqual(owner.handle, 0)

    def test_source_close_failure_preserves_prior_error(self):
        class TwoFailures(FakeApi):
            def _op(self, name):
                self.operations.append(name)
                if name in ('set-medium', 'close-1001'): raise token.TokenSafetyError('injected-'+name)
        api = TwoFailures(); owner = token.RestrictedMediumToken(_api=api)
        with self.assertRaisesRegex(token.TokenSafetyError, 'injected-set-medium') as context: owner.__enter__()
        self.assertIn(2000, api.closed)
        self.assertTrue(any('source-token-close' in note for note in context.exception.__notes__))

    def test_native_restrict_sid_cleanup_failure_closes_untransferred_handle(self):
        # Exercise the production wrapper with fake callable entrypoints, not
        # a native token. The result handle exists before LocalFree can fail.
        api = object.__new__(token._NativeTokenApi); closed = []
        api._sid = lambda text: ctypes.c_void_p(123)
        def fail_free(pointer): raise token.TokenSafetyError('sid-free-failure')
        def created(*args):
            ctypes.cast(args[-1], ctypes.POINTER(token.wt.HANDLE)).contents.value = 2000
            return True
        api._free = fail_free; api.close = closed.append
        api.a = SimpleNamespace(CreateRestrictedToken=created)
        with self.assertRaisesRegex(token.TokenSafetyError, '^sid-free-failure$'): api.restrict(1001)
        self.assertEqual(closed, [2000])

    def test_native_restrict_creation_error_survives_sid_cleanup_error(self):
        api = object.__new__(token._NativeTokenApi); closed = []
        api._sid = lambda text: ctypes.c_void_p(123)
        def fail_free(pointer): raise token.TokenSafetyError('sid-free-failure')
        def failed(*args): raise token.TokenSafetyError('native-restrict-failure')
        api._free = fail_free; api.close = closed.append
        api.a = SimpleNamespace(CreateRestrictedToken=failed)
        with self.assertRaisesRegex(token.TokenSafetyError, '^native-restrict-failure$') as context: api.restrict(1001)
        self.assertEqual(closed, [])
        self.assertTrue(any('admin-SID-free' in note for note in context.exception.__notes__))

    def test_token_has_restrictions_is_one_byte_boolean(self):
        api = object.__new__(token._NativeTokenApi)
        for value in (0, 1):
            api._query = lambda handle, kind, value=value: ctypes.create_string_buffer(bytes([value]), 1)
            self.assertEqual(api._scalar(1001, 21), value)
        for size in (0, 2, 4, 8):
            api._query = lambda handle, kind, size=size: ctypes.create_string_buffer(size)
            with self.subTest(size=size), self.assertRaises(token.TokenSafetyError): api._scalar(1001, 21)

    def test_other_token_scalars_stay_dword(self):
        api = object.__new__(token._NativeTokenApi)
        for kind in (12, 20, 26, 29, 15):
            api._query = lambda handle, info: ctypes.create_string_buffer(b'\x01\x00\x00\x00', 4)
            self.assertEqual(api._scalar(1001, kind), 1)
            for size in (0, 1, 2, 8):
                api._query = lambda handle, info, size=size: ctypes.create_string_buffer(size)
                with self.subTest(kind=kind, size=size), self.assertRaises(token.TokenSafetyError): api._scalar(1001, kind)

    def test_create_privilege_failure_has_no_fallback(self):
        api = FakeApi(); api.fail = 'create-restricted'
        with token.RestrictedMediumToken(_api=api) as owner:
            with self.assertRaisesRegex(token.TokenSafetyError, 'injected-create-restricted'): create(owner)
        self.assertEqual(api.operations.count('create-restricted'), 1)
        self.assertFalse(owner.receipt['childTokenVerifiedBeforeResume'])

    def test_child_query_failure_does_not_verify(self):
        api = FakeApi(); api.fail = 'describe-child'
        with token.RestrictedMediumToken(_api=api) as owner:
            create(owner)
            with self.assertRaisesRegex(token.TokenSafetyError, 'injected-describe-child'): owner.verify_child(999)
        self.assertIn(3000, api.closed); self.assertFalse(owner.receipt['childTokenVerifiedBeforeResume'])

    def test_child_wrong_identity_or_il_refused(self):
        for field, value in (('integritySid', 'S-1-16-12288'), ('integrityRid', 12288), ('sessionId', 2),
            ('authenticationId', 999), ('userSid', 'S-1-5-21-100-200-300-501'), ('uiAccess', True)):
            api = FakeApi(); api.child[field] = value
            with token.RestrictedMediumToken(_api=api) as owner:
                create(owner)
                with self.subTest(field=field), self.assertRaises(token.TokenSafetyError): owner.verify_child(999)
            self.assertFalse(owner.receipt['childTokenVerifiedBeforeResume'])

    def test_kernel_duplicate_child_token_id_allowed_but_recorded(self):
        api = FakeApi(); api.child['tokenId'] = 555
        with token.RestrictedMediumToken(_api=api) as owner:
            create(owner); owner.verify_child(999)
            self.assertEqual(owner.receipt['childBeforeResume']['tokenId'], 555)

    def test_parent_security_change_fails_close(self):
        api = FakeApi(); api.changed_parent = True; owner = token.RestrictedMediumToken(_api=api)
        with self.assertRaisesRegex(token.TokenSafetyError, 'caller token security state changed'):
            with owner: create(owner)
        self.assertIn(2000, api.closed); self.assertFalse(owner.receipt['parentTokenUnchanged'])

    def test_parent_modified_id_is_not_permissions(self):
        api = FakeApi()
        with token.RestrictedMediumToken(_api=api) as owner:
            api.source['modifiedId'] += 1
        self.assertTrue(owner.receipt['parentTokenUnchanged'])

    def test_source_defaults_are_read_before_derivation(self):
        api = FakeApi()
        with token.RestrictedMediumToken(_api=api): pass
        for kind in (4, 5, 6):
            self.assertLess(api.operations.index('query-source-' + str(kind)), api.operations.index('restrict'))
        self.assertLess(api.operations.index('set-default-dacl'), api.operations.index('close-1001'))

    def test_source_default_change_during_derivation_prevents_ready(self):
        class ChangedDuringRestrict(FakeApi):
            def restrict(self, source):
                result = super().restrict(source)
                self.source_default['ownerSid'] = self.source['userSid']
                return result
        api = ChangedDuringRestrict(); owner = token.RestrictedMediumToken(_api=api)
        with self.assertRaisesRegex(dacl.DefaultDaclError, 'since pre-derive'): owner.__enter__()
        self.assertFalse(owner.receipt['ready']); self.assertFalse(owner.receipt['parentTokenUnchanged'])
        self.assertFalse(owner.receipt['defaultDacl']['sourceUnchanged'])
        self.assertEqual(api.default_writes, []); self.assertNotIn('create-restricted', api.operations)
        self.assertIn(2000, api.closed)

    def test_source_default_changes_after_create_fail_final_close(self):
        for field, value in (('ownerSid', 'S-1-5-18'), ('primaryGroupSid', 'S-1-5-18'),
                             ('defaultDacl', dacl.plan_user_object_dacl(basic_acl(), snapshot()['userSid']).after)):
            api = FakeApi(); owner = token.RestrictedMediumToken(_api=api)
            with self.subTest(field=field), self.assertRaisesRegex(token.TokenSafetyError, 'final source default security changed'):
                with owner:
                    create(owner)
                    api.source_default[field] = value
            self.assertFalse(owner.receipt['parentTokenUnchanged']); self.assertIn(2000, api.closed)

    def test_actual_child_default_drift_prevents_verification(self):
        for field, value in (('ownerSid', token.ADMIN_SID), ('primaryGroupSid', 'S-1-5-18'), ('defaultDacl', basic_acl())):
            api = FakeApi()
            with token.RestrictedMediumToken(_api=api) as owner:
                create(owner); api.child_default[field] = value
                with self.subTest(field=field), self.assertRaisesRegex(dacl.DefaultDaclError, 'actual child Owner/Group/DefaultDacl'):
                    owner.verify_child(999)
                self.assertFalse(owner.receipt['childTokenVerifiedBeforeResume'])
                self.assertIn('childDefaultBeforeResume', owner.receipt)
                self.assertIn(3000, api.closed)

    def test_child_default_query_failure_does_not_verify(self):
        api = FakeApi()
        with token.RestrictedMediumToken(_api=api) as owner:
            create(owner); api.fail = 'query-child-6'
            with self.assertRaisesRegex(token.TokenSafetyError, 'injected-query-child-6'): owner.verify_child(999)
            self.assertFalse(owner.receipt['childTokenVerifiedBeforeResume'])
            self.assertIn(3000, api.closed)

    def test_default_dacl_set_failure_has_receipt_no_create_and_closes(self):
        api = FakeApi(); api.fail = 'set-default-dacl'; owner = token.RestrictedMediumToken(_api=api)
        with self.assertRaisesRegex(dacl.DefaultDaclError, 'injected-set-default-dacl'): owner.__enter__()
        self.assertFalse(owner.receipt['ready']); self.assertFalse(owner.receipt['defaultDacl']['ok'])
        self.assertTrue(owner.receipt['defaultDacl']['sourceUnchanged'])
        self.assertNotIn('create-restricted', api.operations); self.assertIn(2000, api.closed)

    def test_default_dacl_setter_success_is_not_readback_proof(self):
        class FailedReadback(FakeApi):
            def _set_token_information(self, *args):
                result = super()._set_token_information(*args)
                self.derived_default['defaultDacl'] = basic_acl()
                return result
        api = FailedReadback(); owner = token.RestrictedMediumToken(_api=api)
        with self.assertRaisesRegex(dacl.DefaultDaclError, 'readback differs'): owner.__enter__()
        self.assertFalse(owner.receipt['ready']); self.assertNotIn('create-restricted', api.operations)
        self.assertFalse(owner.receipt['defaultDacl']['derivedReadbackVerified']); self.assertIn(2000, api.closed)

    def test_existing_user_default_ga_is_idempotent(self):
        api = FakeApi()
        api.derived_default['defaultDacl'] = dacl.plan_user_object_dacl(basic_acl(), api.derived['userSid']).after
        with token.RestrictedMediumToken(_api=api) as owner:
            create(owner); owner.verify_child(999)
        self.assertEqual(api.default_writes, []); self.assertFalse(owner.receipt['defaultDacl']['changed'])
        self.assertIsNone(owner.receipt['defaultDacl']['classWritten'])

    def test_cleanup_error_does_not_replace_body_error(self):
        api = FakeApi(); owner = token.RestrictedMediumToken(_api=api)
        with self.assertRaisesRegex(RuntimeError, '^body-error$') as context:
            with owner:
                api.fail = 'close-2000'
                raise RuntimeError('body-error')
        self.assertTrue(owner.receipt['cleanupErrors'])
        self.assertTrue(any('derived-token-close' in note for note in context.exception.__notes__))

    def test_observation_schema_and_numeric_types(self):
        for field in snapshot():
            row = snapshot(); del row[field]
            with self.subTest(field=field), self.assertRaises(token.TokenSafetyError): token.validate_observation(row)
        for field in ('sessionId', 'tokenType', 'tokenId', 'authenticationId', 'modifiedId', 'integrityRid'):
            for bad in (True, -1, 1.0, 1 << 64):
                row = snapshot(); row[field] = bad
                with self.subTest(field=field, bad=bad), self.assertRaises(token.TokenSafetyError): token.validate_observation(row)
        row = snapshot(); row['extra'] = 1
        with self.assertRaises(token.TokenSafetyError): token.validate_observation(row)

    def test_observation_booleans_do_not_accept_integers(self):
        for field in ('elevated', 'uiAccess', 'appContainer', 'sandboxInert', 'hasRestrictions', 'handleInheritable'):
            row = snapshot(); row[field] = 0
            with self.subTest(field=field), self.assertRaises(token.TokenSafetyError): token.validate_observation(row)

    def test_admin_deny_only_is_actual_not_requested(self):
        for attributes in (7, 0, 0x14, 0x12):
            reduced = snapshot(True); reduced['groups'][0]['attributes'] = attributes
            with self.subTest(attributes=attributes), self.assertRaises(token.TokenSafetyError):
                token.validate_restricted(snapshot(), reduced)

    def test_no_privilege_or_group_upgrade(self):
        reduced = snapshot(True); reduced['privileges'].append(dict(name='SeDebugPrivilege', attributes=0))
        with self.assertRaises(token.TokenSafetyError): token.validate_restricted(snapshot(), reduced)
        reduced = snapshot(True); reduced['groups'].append(dict(sid='S-1-5-32-551', attributes=7))
        with self.assertRaises(token.TokenSafetyError): token.validate_restricted(snapshot(), reduced)
        source = snapshot(); source['groups'][1]['attributes'] = 0x10
        with self.assertRaises(token.TokenSafetyError): token.validate_restricted(source, snapshot(True))

    def test_current_low_integrity_never_raised(self):
        api = FakeApi(); api.source.update(integritySid='S-1-16-4096', integrityRid=4096)
        with self.assertRaises(token.TokenSafetyError): token.RestrictedMediumToken(_api=api).__enter__()
        self.assertNotIn('restrict', api.operations)

    def test_already_medium_regular_user_can_still_restrict(self):
        source = snapshot(); source.update(integritySid=token.MEDIUM_SID, integrityRid=8192)
        reduced = snapshot(True)
        source['groups'] = [dict(sid='S-1-5-32-545', attributes=7)]
        reduced['groups'] = copy.deepcopy(source['groups'])
        token.validate_restricted(source, reduced)

    def test_inert_uiaccess_appcontainer_not_silently_allowed(self):
        for field in ('sandboxInert', 'uiAccess', 'appContainer', 'handleInheritable'):
            reduced = snapshot(True); reduced[field] = True
            with self.subTest(field=field), self.assertRaises(token.TokenSafetyError): token.validate_restricted(snapshot(), reduced)

    def test_launcher_adopts_before_verification_and_no_uncontained_retry(self):
        # Static integration shape supplements pure token-owner tests; it is
        # explicitly not a native process or resource-lifetime proof.
        source = (Path(__file__).parent/'isolated_render_desktop.py').read_text('utf-8')
        call = source.index('reduced_token.create_suspended(')
        adopt = source.index('process, thread = info.hProcess, info.hThread', call)
        job = source.index('api.k.IsProcessInJob(process, job', adopt)
        verify = source.index('reduced_token.verify_child(process)', adopt)
        resume = source.index('api.k.ResumeThread(thread)', verify)
        self.assertLess(call, adopt); self.assertLess(adopt, job); self.assertLess(job, verify); self.assertLess(verify, resume)
        self.assertIn('previous_suspend_count == 1', source[resume:resume+400])
        self.assertNotIn('CreateProcessWithToken', source)
        helper = (Path(__file__).parent/'render_lab_token.py').read_text('utf-8')
        self.assertNotIn('AdjustTokenPrivileges(', helper)
        self.assertNotIn('ImpersonateLoggedOnUser(', helper)
        self.assertNotIn('ShellExecute', helper)


if __name__ == '__main__':
    unittest.main(verbosity=2)
