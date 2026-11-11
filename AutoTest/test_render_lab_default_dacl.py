"""Pure ACL/buffer/guard tests; no token, process, desktop or native API calls."""
import copy
import ctypes as ct
import json
import struct
from types import SimpleNamespace
import unittest

import render_lab_default_dacl as dacl
from test_render_lab_token import FakeApi, basic_acl, pointed_buffer, snapshot


USER = snapshot()['userSid']


def changed(data, offset, fmt, value):
    result = bytearray(data)
    struct.pack_into(fmt, result, offset, value)
    return bytes(result)


def ensure(api):
    # These handles belong only to the injected in-memory fake, never Windows.
    return dacl.ensure_derived_default_dacl(api, 1001, 2000, copy.deepcopy(api.source), copy.deepcopy(api.derived))


class AclPlanTests(unittest.TestCase):
    def test_sid_roundtrip_and_maximum(self):
        for text in ('S-1-0', 'S-1-5-18', USER, 'S-1-281474976710655' + '-4294967295' * 15):
            data = dacl.encode_sid(text)
            self.assertEqual(dacl.decode_sid(data, 0, len(data)), (text, len(data)))
            self.assertLessEqual(len(data), 68)

    def test_sid_noncanonical_and_numeric_bounds_refused(self):
        for text in (None, '', 'S-2-5-18', 'S-1-05-18', 'S-1-5-018', 'S-1--5-18',
                     'S-1-281474976710656', 'S-1-5-4294967296', 'S-1-5' + '-1' * 16, 'S-1-5-1\n'):
            with self.subTest(text=text), self.assertRaises(dacl.DefaultDaclError): dacl.encode_sid(text)

    def test_sid_malformed_buffer_refused(self):
        data = dacl.encode_sid(USER)
        for buffer, offset, end in ((data, -1, len(data)), (data, 0, len(data) + 1),
            (data, 0, len(data) - 1), (b'\x01\x10' + b'\0' * 70, 0, 72),
            (b'\x02' + data[1:], 0, len(data)), (data, len(data), len(data))):
            with self.subTest(offset=offset, end=end), self.assertRaises(dacl.DefaultDaclError):
                dacl.decode_sid(buffer, offset, end)

    def test_three_aces_preserved_bytewise_and_exact_one_user_ga(self):
        before = basic_acl(); plan = dacl.plan_user_object_dacl(before, USER)
        old = dacl.parse_simple_acl(before); new = dacl.parse_simple_acl(plan.after)
        self.assertTrue(plan.changed); self.assertEqual(plan.before, before)
        self.assertEqual(new.entries[:-1], old.entries)
        self.assertEqual(new.entries[-1].sid, USER); self.assertEqual(new.entries[-1].mask, dacl.GENERIC_ALL)
        self.assertEqual(plan.after[8:len(before)], before[8:])
        self.assertEqual(len(new.entries), 4)
        self.assertEqual(new.entries[2].mask, 0xA0000000)  # logon is NOT broadened

    def test_unused_acl_bytes_and_revision_are_preserved(self):
        before = basic_acl() + b'\xff\xfe\xfd\xfc' * 3
        before = changed(changed(before, 2, '<H', len(before)), 0, '<B', 4)
        plan = dacl.plan_user_object_dacl(before, USER)
        parsed = dacl.parse_simple_acl(plan.after)
        self.assertEqual(parsed.revision, 4); self.assertEqual(parsed.padding, b'\xff\xfe\xfd\xfc' * 3)
        self.assertEqual(parsed.entries[:-1], dacl.parse_simple_acl(before).entries)

    def test_existing_user_ga_exact_noop(self):
        first = dacl.plan_user_object_dacl(basic_acl(), USER)
        second = dacl.plan_user_object_dacl(first.after, USER)
        self.assertFalse(second.changed); self.assertEqual(second.before, second.after)

    def test_user_readonly_does_not_count_as_ga(self):
        first = dacl.plan_user_object_dacl(basic_acl(), USER).after
        offset = len(basic_acl()) + 4
        readonly = changed(first, offset, '<I', 0x80000000)
        plan = dacl.plan_user_object_dacl(readonly, USER)
        parsed = dacl.parse_simple_acl(plan.after)
        self.assertTrue(plan.changed); self.assertEqual(parsed.entries[-2].mask, 0x80000000)
        self.assertEqual(parsed.entries[-1].mask, dacl.GENERIC_ALL)

    def test_empty_nonnull_acl_can_be_narrowly_filled(self):
        data = struct.pack('<BBHHH', 2, 0, 8, 0, 0)
        parsed = dacl.parse_simple_acl(dacl.plan_user_object_dacl(data, USER).after)
        self.assertEqual(len(parsed.entries), 1); self.assertEqual(parsed.entries[0].sid, USER)

    def test_null_or_nonbytes_acl_refused(self):
        for data in (None, b'', b'\0' * 7, bytearray(basic_acl()), 'D:(A;;GA;;;BA)'):
            with self.subTest(data=type(data)), self.assertRaises(dacl.DefaultDaclError):
                dacl.plan_user_object_dacl(data, USER)

    def test_invalid_acl_headers_refused(self):
        original = basic_acl()
        for offset, fmt, value in ((0, '<B', 1), (0, '<B', 3), (1, '<B', 1), (6, '<H', 1),
                                   (2, '<H', len(original) - 1), (4, '<H', 65535)):
            with self.subTest(offset=offset, value=value), self.assertRaises(dacl.DefaultDaclError):
                dacl.parse_simple_acl(changed(original, offset, fmt, value))

    def test_deny_unknown_object_callback_and_inheriting_aces_refused(self):
        original = basic_acl()
        for kind in (1, 2, 5, 6, 9, 10, 17, 255):
            with self.subTest(kind=kind), self.assertRaises(dacl.DefaultDaclError):
                dacl.parse_simple_acl(changed(original, 8, '<B', kind))
        for flags in (1, 2, 4, 8, 0x10, 0x40, 0x80, 255):
            with self.subTest(flags=flags), self.assertRaises(dacl.DefaultDaclError):
                dacl.parse_simple_acl(changed(original, 9, '<B', flags))

    def test_ace_extent_and_sid_bounds_refused(self):
        original = basic_acl()
        for length in (0, 4, 12, 15, 17, 65532):
            with self.subTest(length=length), self.assertRaises(dacl.DefaultDaclError):
                dacl.parse_simple_acl(changed(original, 10, '<H', length))
        for count in (0, 15, 16):
            with self.subTest(count=count), self.assertRaises(dacl.DefaultDaclError):
                dacl.parse_simple_acl(changed(original, 17, '<B', count))

    def test_acl_expansion_overflow_refused(self):
        # Valid explicit empty ACL with slack; append cannot exceed USHORT.
        data = struct.pack('<BBHHH', 2, 0, 65532, 0, 0) + b'\0' * (65532 - 8)
        dacl.parse_simple_acl(data)
        with self.assertRaisesRegex(dacl.DefaultDaclError, 'expanded ACL bound'):
            dacl.plan_user_object_dacl(data, USER)

    def test_summary_is_json_safe_and_unsupported_visible(self):
        self.assertEqual(dacl.acl_summary(None), {'null': True})
        row = dacl.acl_summary(basic_acl()); self.assertEqual(row['aceCount'], 3)
        self.assertIn('unsupportedReason', dacl.acl_summary(b'bad'))
        json.dumps(row)


class NativeWrapperPureTests(unittest.TestCase):
    def test_constructor_has_no_native_calls(self):
        api = FakeApi()
        dacl.NativeDefaultDaclApi(api, 1001, 2000, api.source, api.derived)
        self.assertEqual(api.operations, [])

    def test_queries_are_bounded_owner_group_and_dacl(self):
        api = FakeApi(); row = dacl.inspect_default_security(api, 2000)
        self.assertEqual(row['ownerSid'], USER); self.assertEqual(row['primaryGroupSid'], 'S-1-5-32-545')
        self.assertEqual(row['defaultDacl'], basic_acl())
        self.assertEqual(api.operations, ['describe-derived', 'query-derived-4', 'query-derived-5', 'query-derived-6'])

    def test_pointer_bounds_and_missing_owner_refused(self):
        api = FakeApi()
        for mode in ('before-header', 'past-end', 'null', 'short-buffer'):
            def query(handle, kind):
                if mode == 'short-buffer': return ct.create_string_buffer(ct.sizeof(ct.c_void_p) - 1)
                result = pointed_buffer(dacl.encode_sid(USER))
                address = ct.addressof(result)
                ct.c_void_p.from_buffer(result).value = {'before-header': address, 'past-end': address + ct.sizeof(result), 'null': None}[mode]
                return result
            api._query = query
            with self.subTest(mode=mode), self.assertRaises(dacl.DefaultDaclError): dacl.inspect_default_security(api, 2000)

    def test_acl_native_extent_refused(self):
        api = FakeApi(); original_query = api._query
        api._query = lambda handle, kind: (pointed_buffer(changed(basic_acl(), 2, '<H', 65535))
                                           if kind == 6 else original_query(handle, kind))
        with self.assertRaisesRegex(dacl.DefaultDaclError, 'native extent'): dacl.inspect_default_security(api, 2000)

    def test_null_default_acl_visible_but_repair_refused(self):
        api = FakeApi(); api.derived_default['defaultDacl'] = None
        self.assertIsNone(dacl.inspect_default_security(api, 2000)['defaultDacl'])
        with self.assertRaisesRegex(dacl.DefaultDaclError, 'non-null'): ensure(api)
        self.assertEqual(api.default_writes, [])

    def test_only_class6_pointer_sized_exact_plan_written(self):
        api = FakeApi(); wrapper = dacl.NativeDefaultDaclApi(api, 1001, 2000, api.source, api.derived)
        plan = dacl.plan_user_object_dacl(basic_acl(), USER)
        wrapper.set_derived_default_dacl(2000, plan.before, plan.after, USER)
        self.assertEqual(api.default_writes, [(2000, 6, plan.after)])
        self.assertEqual(api.source_default['defaultDacl'], plan.before)

    def test_bound_source_handle_or_token_alias_refused(self):
        api = FakeApi(); plan = dacl.plan_user_object_dacl(basic_acl(), USER)
        wrapper = dacl.NativeDefaultDaclApi(api, 1001, 2000, api.source, api.derived)
        for handle in (1001, 3000, 0):
            with self.subTest(handle=handle), self.assertRaises(dacl.DefaultDaclError):
                wrapper.set_derived_default_dacl(handle, plan.before, plan.after, USER)
        api.derived['tokenId'] = api.source['tokenId']
        wrapper = dacl.NativeDefaultDaclApi(api, 1001, 2000, api.source, api.derived)
        with self.assertRaisesRegex(dacl.DefaultDaclError, 'source token alias'):
            wrapper.set_derived_default_dacl(2000, plan.before, plan.after, USER)
        self.assertEqual(api.default_writes, [])

    def test_arbitrary_acl_wrong_user_and_stale_before_refused(self):
        plan = dacl.plan_user_object_dacl(basic_acl(), USER)
        for before, after, user in ((plan.before, plan.before, USER), (plan.after, plan.after, USER),
            (plan.before, plan.after, 'S-1-5-18'), (plan.before, changed(plan.after, 12, '<I', 0xffffffff), USER)):
            api = FakeApi(); wrapper = dacl.NativeDefaultDaclApi(api, 1001, 2000, api.source, api.derived)
            with self.subTest(user=user, size=len(before)), self.assertRaises(dacl.DefaultDaclError):
                wrapper.set_derived_default_dacl(2000, before, after, user)
            self.assertEqual(api.default_writes, [])

    def test_fresh_identity_drift_prevents_write(self):
        api = FakeApi(); wrapper = dacl.NativeDefaultDaclApi(api, 1001, 2000, api.source, api.derived)
        api.derived['integrityRid'] = 12288
        plan = dacl.plan_user_object_dacl(basic_acl(), USER)
        with self.assertRaisesRegex(dacl.DefaultDaclError, 'identity/security drift'):
            wrapper.set_derived_default_dacl(2000, plan.before, plan.after, USER)
        self.assertEqual(api.default_writes, [])


class GuardAndReadbackTests(unittest.TestCase):
    def test_full_fake_repair_has_verified_source_and_derived_receipt(self):
        api = FakeApi(); receipt = ensure(api)
        self.assertTrue(receipt['ok']); self.assertTrue(receipt['changed'])
        self.assertTrue(receipt['sourceUnchanged']); self.assertTrue(receipt['derivedReadbackVerified'])
        self.assertFalse(receipt['sourceTokenWritten']); self.assertFalse(receipt['sharedObjectAclChanged'])
        self.assertEqual(receipt['derivedAfter']['defaultDacl']['aceCount'], 4)
        self.assertEqual(receipt['sourceBefore'], receipt['sourceAfter'])
        json.dumps(receipt)

    def test_identity_and_restriction_preconditions_no_write(self):
        for field, value in (('tokenId', 111), ('tokenType', 2), ('userSid', 'S-1-5-18'), ('authenticationId', 334),
            ('sessionId', 2), ('integrityRid', 12288), ('hasRestrictions', False), ('handleInheritable', True),
            ('uiAccess', True), ('appContainer', True), ('sandboxInert', True)):
            api = FakeApi(); api.derived[field] = value
            with self.subTest(field=field), self.assertRaises(dacl.DefaultDaclError) as context: ensure(api)
            self.assertEqual(api.default_writes, []); self.assertTrue(context.exception.receipt['sourceUnchanged'])

    def test_privilege_admin_and_owner_not_repaired_or_broadened(self):
        for field in ('privilege', 'admin', 'owner'):
            api = FakeApi()
            if field == 'privilege': api.derived['privileges'].append(dict(name='SeDebugPrivilege', attributes=0))
            if field == 'admin': api.derived['groups'][0]['attributes'] = 7
            if field == 'owner': api.derived_default['ownerSid'] = dacl.ADMIN_SID
            with self.subTest(field=field), self.assertRaises(dacl.DefaultDaclError): ensure(api)
            self.assertEqual(api.default_writes, [])

    def test_observation_drift_fails_before_mutation(self):
        for field in ('source', 'derived'):
            api = FakeApi(); a = copy.deepcopy(api.source); b = copy.deepcopy(api.derived)
            getattr(api, field)['tokenId'] += 1
            with self.subTest(field=field), self.assertRaisesRegex(dacl.DefaultDaclError, 'observation drift'):
                dacl.ensure_derived_default_dacl(api, 1001, 2000, a, b)
            self.assertEqual(api.default_writes, [])

    def test_same_handle_and_invalid_handles_fail_without_query(self):
        for source, derived in ((1001, 1001), (0, 2000), (1001, -1), (True, 2000)):
            api = FakeApi()
            with self.subTest(source=source, derived=derived), self.assertRaises(dacl.DefaultDaclError):
                dacl.ensure_derived_default_dacl(api, source, derived, api.source, api.derived)
            self.assertEqual(api.operations, [])

    def test_setter_failure_still_reads_source_after(self):
        api = FakeApi(); api.fail = 'set-default-dacl'
        with self.assertRaises(dacl.DefaultDaclError) as context: ensure(api)
        receipt = context.exception.receipt
        self.assertFalse(receipt['ok']); self.assertTrue(receipt['sourceUnchanged'])
        self.assertFalse(receipt['derivedReadbackVerified']); self.assertIn('sourceAfter', receipt)
        self.assertGreater(api.operations.index('query-source-6', api.operations.index('set-default-dacl')), 0)

    def test_derived_owner_group_or_profile_change_in_setter_refused(self):
        for field in ('ownerSid', 'primaryGroupSid', 'privilege'):
            api = FakeApi(); setter = api.a.SetTokenInformation
            def mutate(*args):
                result = setter(*args)
                if field == 'privilege': api.derived['privileges'] = []
                else: api.derived_default[field] = 'S-1-5-18'
                return result
            api.a = SimpleNamespace(SetTokenInformation=mutate)
            with self.subTest(field=field), self.assertRaises(dacl.DefaultDaclError) as context: ensure(api)
            self.assertFalse(context.exception.receipt['derivedReadbackVerified'])
            self.assertTrue(context.exception.receipt['sourceUnchanged'])

    def test_source_change_in_setter_is_detected_even_with_exact_derived(self):
        api = FakeApi(); setter = api.a.SetTokenInformation
        def mutate(*args):
            result = setter(*args); api.source_default['primaryGroupSid'] = 'S-1-5-18'; return result
        api.a = SimpleNamespace(SetTokenInformation=mutate)
        with self.assertRaisesRegex(dacl.DefaultDaclError, 'source token Owner/Group/DefaultDacl/security changed') as context:
            ensure(api)
        self.assertTrue(context.exception.receipt['derivedReadbackVerified'])
        self.assertFalse(context.exception.receipt['sourceUnchanged']); self.assertFalse(context.exception.receipt['ok'])

    def test_prederive_baseline_is_checked_not_replaced(self):
        api = FakeApi(); baseline = dacl.inspect_default_security(api, 1001)
        api.source_default['ownerSid'] = USER
        with self.assertRaisesRegex(dacl.DefaultDaclError, 'pre-derive read') as context:
            dacl.ensure_derived_default_dacl(api, 1001, 2000, api.source, api.derived, source_default_before=baseline)
        self.assertFalse(context.exception.receipt['sourceUnchanged']); self.assertEqual(api.default_writes, [])

    def test_final_source_and_actual_child_must_match_verified_defaults(self):
        api = FakeApi(); receipt = ensure(api)
        dacl.assert_source_default_security(dacl.inspect_default_security(api, 1002), receipt)
        api.child_default = copy.deepcopy(api.derived_default); api.child['tokenId'] = 999
        dacl.assert_child_default_security(dacl.inspect_default_security(api, 3000), receipt)
        for field, value in (('ownerSid', dacl.ADMIN_SID), ('primaryGroupSid', 'S-1-5-18'), ('defaultDacl', basic_acl())):
            api.child_default = copy.deepcopy(api.derived_default); api.child_default[field] = value
            with self.subTest(field=field), self.assertRaises(dacl.DefaultDaclError):
                dacl.assert_child_default_security(dacl.inspect_default_security(api, 3000), receipt)
        api.source_default['ownerSid'] = USER
        with self.assertRaises(dacl.DefaultDaclError):
            dacl.assert_source_default_security(dacl.inspect_default_security(api, 1002), receipt)

    def test_unverified_receipt_never_certifies_child(self):
        api = FakeApi(); row = dacl.inspect_default_security(api, 3000)
        for receipt in ({}, {'ok': False}, {'ok': True, 'derivedReadbackVerified': False}):
            with self.subTest(receipt=receipt), self.assertRaises(dacl.DefaultDaclError):
                dacl.assert_child_default_security(row, receipt)


if __name__ == '__main__':
    unittest.main(verbosity=2)
