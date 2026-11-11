"""Opt-in restricted Medium token for ONE owned non-input-desktop lab child.

No module import side effects, global UAC/registry/ACL change, impersonation,
privilege adjustment, alternate credentials, shell, or fallback to High IL.
CreateRestrictedToken derives the caller's own primary token; only the derived
handle receives SetTokenInformation. This is NOT a general security sandbox.

The launcher retains its atomic Job + handle whitelist. The child must start
suspended, be token/identity checked, and only then resume. ERROR_PRIVILEGE_NOT_HELD
is a blocked launch, not permission to enable privileges or change the system.
"""
from __future__ import annotations

import copy
import ctypes
from ctypes import wintypes as wt
import os
import re
from typing import Any

import render_lab_default_dacl as default_dacl


ADMIN_SID = 'S-1-5-32-544'
MEDIUM_SID = 'S-1-16-8192'
MEDIUM_RID = 0x2000
DISABLE_MAX_PRIVILEGE = 1
DENY_ONLY = 0x10
ENABLED = 0x4
ENABLED_BY_DEFAULT = 0x2
CREATE_SUSPENDED = 0x4
BASE_FLAGS = 0x08000000 | 0x4000 | 0x80000 | 0x400
TOKEN_ACCESS = 0x1 | 0x2 | 0x8 | 0x80  # assign-primary, duplicate, query, adjust-default


class TokenSafetyError(RuntimeError):
    pass


def require(value: Any, message: str) -> None:
    if not value:
        raise TokenSafetyError(message)


class _SidAndAttributes(ctypes.Structure):
    _fields_ = [('Sid', wt.LPVOID), ('Attributes', wt.DWORD)]


class _Luid(ctypes.Structure):
    _fields_ = [('LowPart', wt.DWORD), ('HighPart', wt.LONG)]


class _LuidAndAttributes(ctypes.Structure):
    _fields_ = [('Luid', _Luid), ('Attributes', wt.DWORD)]


class _Statistics(ctypes.Structure):
    _fields_ = [('TokenId', _Luid), ('AuthenticationId', _Luid), ('ExpirationTime', ctypes.c_int64),
        ('TokenType', wt.DWORD), ('ImpersonationLevel', wt.DWORD), ('DynamicCharged', wt.DWORD),
        ('DynamicAvailable', wt.DWORD), ('GroupCount', wt.DWORD), ('PrivilegeCount', wt.DWORD), ('ModifiedId', _Luid)]


class _GroupHead(ctypes.Structure):
    _fields_ = [('count', wt.DWORD), ('first', _SidAndAttributes)]


class _PrivilegeHead(ctypes.Structure):
    _fields_ = [('count', wt.DWORD), ('first', _LuidAndAttributes)]


def _luid(value: _Luid) -> int:
    return int(value.LowPart) | ((int(value.HighPart) & 0xffffffff) << 32)


def security_profile(row: dict[str, Any]) -> dict[str, Any]:
    """Security state only. Native temporary privilege use may change ModifiedId."""
    return {key: copy.deepcopy(row[key]) for key in ('userSid', 'sessionId', 'tokenType', 'authenticationId',
        'integritySid', 'integrityRid', 'groups', 'privileges', 'elevated', 'uiAccess', 'appContainer', 'sandboxInert', 'hasRestrictions')}


def validate_observation(row: dict[str, Any]) -> None:
    expected = {'userSid', 'sessionId', 'tokenType', 'tokenId', 'authenticationId', 'modifiedId',
        'integritySid', 'integrityRid', 'groups', 'privileges', 'elevated', 'uiAccess', 'appContainer',
        'sandboxInert', 'hasRestrictions', 'handleInheritable'}
    require(type(row) is dict and set(row) == expected, 'token observation exact schema')
    for key in ('sessionId', 'tokenType', 'integrityRid', 'tokenId', 'authenticationId', 'modifiedId'):
        require(type(row[key]) is int and 0 <= row[key] < (1 << (64 if key.endswith('Id') and key != 'sessionId' else 32)),
                'token unsigned ' + key)
    for key in ('elevated', 'uiAccess', 'appContainer', 'sandboxInert', 'hasRestrictions', 'handleInheritable'):
        require(type(row[key]) is bool, 'token bool ' + key)
    require(type(row['userSid']) is str and re.fullmatch(r'S-1-(?:\d+-)+\d+', row['userSid']), 'token user SID')
    require(row['integritySid'] == 'S-1-16-' + str(row['integrityRid']), 'integrity SID/RID mismatch')
    require(row['tokenType'] == 1 and row['tokenId'] > 0 and row['authenticationId'] > 0, 'primary token identity')
    require(row['handleInheritable'] is False, 'token handle must not be inheritable')
    require(type(row['groups']) is list and type(row['privileges']) is list, 'token arrays')
    sids = []
    for group in row['groups']:
        require(type(group) is dict and set(group) == {'sid', 'attributes'}, 'token group schema')
        require(type(group['sid']) is str and re.fullmatch(r'S-1-(?:\d+-)+\d+', group['sid']), 'group SID')
        require(type(group['attributes']) is int and 0 <= group['attributes'] < 1 << 32, 'group attributes')
        sids.append(group['sid'])
    require(len(set(sids)) == len(sids), 'duplicate group SID')
    names = []
    for privilege in row['privileges']:
        require(type(privilege) is dict and set(privilege) == {'name', 'attributes'}, 'privilege schema')
        require(type(privilege['name']) is str and bool(privilege['name']), 'privilege name')
        require(type(privilege['attributes']) is int and 0 <= privilege['attributes'] < 1 << 32, 'privilege attributes')
        names.append(privilege['name'])
    require(len(set(names)) == len(names), 'duplicate privilege')


def validate_restricted(source: dict[str, Any], reduced: dict[str, Any]) -> None:
    validate_observation(source); validate_observation(reduced)
    require(source['integrityRid'] >= MEDIUM_RID, 'never raise a lower-integrity caller')
    require(reduced['integrityRid'] == MEDIUM_RID and reduced['integritySid'] == MEDIUM_SID,
            'derived token must be exactly Medium')
    for key in ('userSid', 'sessionId', 'authenticationId', 'tokenType'):
        require(reduced[key] == source[key], 'derived token changed caller identity: ' + key)
    require(reduced['tokenId'] != source['tokenId'], 'source token was not duplicated/restricted')
    require(reduced['hasRestrictions'] is True and reduced['uiAccess'] is False
            and reduced['appContainer'] is False and reduced['sandboxInert'] is False,
            'derived token restriction/profile')
    # Do not treat TokenElevation's UAC metadata as the integrity proof. A
    # derived token's actual IL, groups and remaining privileges are queried.
    require(all(p['name'] == 'SeChangeNotifyPrivilege' for p in reduced['privileges']),
            'DISABLE_MAX_PRIVILEGE did not remove privileged rights')
    original = {g['sid']: g['attributes'] for g in source['groups']}
    actual = {g['sid']: g['attributes'] for g in reduced['groups']}
    if ADMIN_SID in original:
        require(ADMIN_SID in actual and actual[ADMIN_SID] & DENY_ONLY
                and not actual[ADMIN_SID] & (ENABLED | ENABLED_BY_DEFAULT), 'Administrators SID must be deny-only')
    else:
        require(ADMIN_SID not in actual, 'derived token introduced Administrators SID')
    other = lambda groups: {sid: attributes for sid, attributes in groups.items()
                            if sid != ADMIN_SID and not sid.startswith('S-1-16-')}
    require(other(actual) == other(original), 'derived token changed another security group or its permissions')


class _NativeTokenApi:
    def __init__(self) -> None:
        require(os.name == 'nt', 'restricted render-lab token is Windows-only')
        self.a = ctypes.WinDLL('advapi32', use_last_error=True)
        self.k = ctypes.WinDLL('kernel32', use_last_error=True)
        signatures = (
            (self.k, 'GetCurrentProcess', wt.HANDLE, ()),
            (self.k, 'CloseHandle', wt.BOOL, (wt.HANDLE,)),
            (self.k, 'LocalFree', wt.HANDLE, (wt.HANDLE,)),
            (self.k, 'GetHandleInformation', wt.BOOL, (wt.HANDLE, ctypes.POINTER(wt.DWORD))),
            (self.a, 'OpenProcessToken', wt.BOOL, (wt.HANDLE, wt.DWORD, ctypes.POINTER(wt.HANDLE))),
            (self.a, 'GetTokenInformation', wt.BOOL, (wt.HANDLE, ctypes.c_int, wt.LPVOID, wt.DWORD, ctypes.POINTER(wt.DWORD))),
            (self.a, 'CreateRestrictedToken', wt.BOOL, (wt.HANDLE, wt.DWORD, wt.DWORD, wt.LPVOID,
                wt.DWORD, wt.LPVOID, wt.DWORD, wt.LPVOID, ctypes.POINTER(wt.HANDLE))),
            (self.a, 'SetTokenInformation', wt.BOOL, (wt.HANDLE, ctypes.c_int, wt.LPVOID, wt.DWORD)),
            (self.a, 'ConvertStringSidToSidW', wt.BOOL, (wt.LPCWSTR, ctypes.POINTER(wt.LPVOID))),
            (self.a, 'ConvertSidToStringSidW', wt.BOOL, (wt.LPVOID, ctypes.POINTER(wt.LPWSTR))),
            (self.a, 'IsValidSid', wt.BOOL, (wt.LPVOID,)),
            (self.a, 'GetLengthSid', wt.DWORD, (wt.LPVOID,)),
            (self.a, 'LookupPrivilegeNameW', wt.BOOL, (wt.LPCWSTR, ctypes.POINTER(_Luid), wt.LPWSTR, ctypes.POINTER(wt.DWORD))),
            (self.a, 'CreateProcessAsUserW', wt.BOOL, (wt.HANDLE, wt.LPCWSTR, wt.LPWSTR, wt.LPVOID, wt.LPVOID,
                wt.BOOL, wt.DWORD, wt.LPVOID, wt.LPCWSTR, wt.LPVOID, wt.LPVOID)),
        )
        for library, name, result, args in signatures:
            function = getattr(library, name); function.restype = result; function.argtypes = args

    @staticmethod
    def check(value: Any, operation: str) -> Any:
        if not value:
            raise TokenSafetyError(f'{operation} failed: Win32={ctypes.get_last_error()}; no privilege/fallback change allowed')
        return value

    def open_process(self, process: int, access: int = 0x8) -> int:
        token = wt.HANDLE()
        self.check(self.a.OpenProcessToken(process, access, ctypes.byref(token)), 'OpenProcessToken')
        return int(token.value)

    def open_current(self) -> int:
        return self.open_process(self.k.GetCurrentProcess(), TOKEN_ACCESS)

    def close(self, token: int) -> None:
        self.check(self.k.CloseHandle(token), 'CloseHandle(token)')

    def _free(self, pointer: Any) -> None:
        require(not self.k.LocalFree(ctypes.cast(pointer, wt.LPVOID)), 'LocalFree(SID) failed')

    def _sid(self, text: str) -> Any:
        pointer = wt.LPVOID()
        self.check(self.a.ConvertStringSidToSidW(text, ctypes.byref(pointer)), 'ConvertStringSidToSidW')
        return pointer

    def _sid_text(self, pointer: int, buffer: Any) -> str:
        start = ctypes.addressof(buffer); end = start+ctypes.sizeof(buffer)
        require(pointer is not None and start <= pointer <= end-8, 'token SID pointer outside returned buffer')
        count = ctypes.c_ubyte.from_address(pointer+1).value
        require(count <= 15 and pointer+8+4*count <= end, 'token SID storage bounds')
        self.check(self.a.IsValidSid(pointer), 'IsValidSid')
        text = wt.LPWSTR()
        self.check(self.a.ConvertSidToStringSidW(pointer, ctypes.byref(text)), 'ConvertSidToStringSidW')
        try: return str(text.value)
        finally: self._free(text)

    def _query(self, token: int, kind: int) -> Any:
        size = wt.DWORD()
        self.a.GetTokenInformation(token, kind, None, 0, ctypes.byref(size))
        require(0 < size.value <= 65536, 'bounded token information size')
        buffer = ctypes.create_string_buffer(size.value)
        self.check(self.a.GetTokenInformation(token, kind, buffer, size.value, ctypes.byref(size)), 'GetTokenInformation')
        require(0 < size.value <= ctypes.sizeof(buffer), 'token information returned length')
        return buffer

    def _scalar(self, token: int, kind: int) -> int:
        buffer = self._query(token, kind)
        # The local API probe returned one byte for TokenHasRestrictions.
        # Keep that measured layout explicit; other scalar classes remain DWORD.
        # This does not assert the same layout on every supported Windows build.
        scalar = ctypes.c_ubyte if kind == 21 else wt.DWORD
        require(ctypes.sizeof(buffer) == ctypes.sizeof(scalar), 'token scalar layout')
        return int(scalar.from_buffer_copy(buffer).value)

    def restrict(self, source: int) -> int:
        admin = self._sid(ADMIN_SID); result = wt.HANDLE()
        try:
            disabled = _SidAndAttributes(admin, 0)
            self.check(self.a.CreateRestrictedToken(source, DISABLE_MAX_PRIVILEGE, 1, ctypes.byref(disabled),
                0, None, 0, None, ctypes.byref(result)), 'CreateRestrictedToken')
        except BaseException as original:
            try: self._free(admin)
            except BaseException as cleanup:
                if hasattr(original, 'add_note'): original.add_note('admin-SID-free:' + str(cleanup))
            raise
        try:
            self._free(admin)
        except BaseException as original:
            # The result has not been handed to RestrictedMediumToken yet.
            # A failed ancillary SID free must not orphan that token handle.
            try: self.close(int(result.value))
            except BaseException as cleanup:
                if hasattr(original, 'add_note'): original.add_note('untransferred-token-close:' + str(cleanup))
            raise
        return int(result.value)

    def set_medium(self, token: int) -> None:
        medium = self._sid(MEDIUM_SID)
        try:
            label = _SidAndAttributes(medium, 0x20)  # SE_GROUP_INTEGRITY
            size = self.a.GetLengthSid(medium)
            require(0 < size <= 68, 'Medium SID size')
            self.check(self.a.SetTokenInformation(token, 25, ctypes.byref(label), ctypes.sizeof(label)+size),
                       'SetTokenInformation(derived Medium token only)')
        finally: self._free(medium)

    def describe(self, token: int) -> dict[str, Any]:
        user = self._query(token, 1); user_sid = self._sid_text(_SidAndAttributes.from_buffer_copy(user).Sid, user)
        integrity = self._query(token, 25)
        integrity_sid = self._sid_text(_SidAndAttributes.from_buffer_copy(integrity).Sid, integrity)
        require(re.fullmatch(r'S-1-16-\d+', integrity_sid), 'native integrity SID')
        stats_buffer = self._query(token, 10)
        require(ctypes.sizeof(stats_buffer) >= ctypes.sizeof(_Statistics), 'token statistics size')
        stats = _Statistics.from_buffer_copy(stats_buffer)
        groups_buffer = self._query(token, 2); count = wt.DWORD.from_buffer_copy(groups_buffer).value
        require(_GroupHead.first.offset+count*ctypes.sizeof(_SidAndAttributes) <= ctypes.sizeof(groups_buffer), 'group count bounds')
        groups = []
        for index in range(count):
            item = _SidAndAttributes.from_buffer_copy(groups_buffer, _GroupHead.first.offset+index*ctypes.sizeof(_SidAndAttributes))
            groups.append(dict(sid=self._sid_text(item.Sid, groups_buffer), attributes=int(item.Attributes)))
        privileges_buffer = self._query(token, 3); count = wt.DWORD.from_buffer_copy(privileges_buffer).value
        require(_PrivilegeHead.first.offset+count*ctypes.sizeof(_LuidAndAttributes) <= ctypes.sizeof(privileges_buffer), 'privilege count bounds')
        privileges = []
        for index in range(count):
            item = _LuidAndAttributes.from_buffer_copy(privileges_buffer, _PrivilegeHead.first.offset+index*ctypes.sizeof(_LuidAndAttributes))
            length = wt.DWORD()
            self.a.LookupPrivilegeNameW(None, ctypes.byref(item.Luid), None, ctypes.byref(length))
            require(0 < length.value <= 256, 'privilege name bound')
            name = ctypes.create_unicode_buffer(length.value+1); length.value = len(name)
            self.check(self.a.LookupPrivilegeNameW(None, ctypes.byref(item.Luid), name, ctypes.byref(length)), 'LookupPrivilegeNameW')
            # USED_FOR_ACCESS is historical accounting, not permission state.
            privileges.append(dict(name=name.value, attributes=int(item.Attributes) & ~0x80000000))
        flags = wt.DWORD(); self.check(self.k.GetHandleInformation(token, ctypes.byref(flags)), 'GetHandleInformation(token)')
        result = dict(userSid=user_sid, integritySid=integrity_sid, integrityRid=int(integrity_sid.rsplit('-', 1)[1]),
            tokenType=int(stats.TokenType), tokenId=_luid(stats.TokenId), authenticationId=_luid(stats.AuthenticationId),
            modifiedId=_luid(stats.ModifiedId), sessionId=self._scalar(token, 12), groups=sorted(groups, key=lambda g:g['sid']),
            privileges=sorted(privileges, key=lambda p:p['name']), handleInheritable=bool(flags.value & 1))
        for key, kind in (('elevated', 20), ('hasRestrictions', 21), ('uiAccess', 26), ('appContainer', 29), ('sandboxInert', 15)):
            value = self._scalar(token, kind); require(value in (0, 1), 'native token boolean ' + key); result[key] = bool(value)
        validate_observation(result)
        return result

    def create(self, token: int, executable: str, command: Any, flags: int, environment: Any,
               directory: str, startup: Any, info: Any) -> None:
        self.check(self.a.CreateProcessAsUserW(token, executable, command, None, None, True, flags,
            environment, directory, ctypes.byref(startup), ctypes.byref(info)), 'CreateProcessAsUserW(restricted Medium)')


class RestrictedMediumToken:
    """Single-use token owner; native API is injectable only for pure tests."""
    def __init__(self, *, _api: Any = None) -> None:
        self._api = _api; self.handle = 0; self._entered = False; self._created = False
        self._source_default_before = None
        self.receipt = dict(schema=1, mode='restricted-medium-per-child/v1', ready=False,
            restrictFlags=DISABLE_MAX_PRIVILEGE, disabledSid=ADMIN_SID, requestedIntegritySid=MEDIUM_SID,
            parentTokenUnchanged=False, derivedTokenClosed=False, childTokenVerifiedBeforeResume=False,
            noHighIntegrityFallback=True, globalSecurityChanged=False, cleanupErrors=[])

    def __enter__(self) -> 'RestrictedMediumToken':
        require(not self._entered, 'token owner is single-use'); self._entered = True
        if self._api is None: self._api = _NativeTokenApi()
        source = 0; original = None
        try:
            source = self._api.open_current(); before = self._api.describe(source)
            validate_observation(before); require(before['integrityRid'] >= MEDIUM_RID, 'do not raise integrity')
            self.receipt['sourceBefore'] = before
            # Read BEFORE deriving or lowering any token. Identity-only checks
            # cannot prove Owner/Group/DefaultDacl on the source stayed intact.
            self._source_default_before = default_dacl.inspect_default_security(self._api, source, identity=before)
            self.receipt['sourceDefaultBefore'] = default_dacl.default_security_summary(self._source_default_before)
            derived = self._api.restrict(source)
            require(derived and derived != source, 'derived token must have distinct owned handle')
            self.handle = derived
            self._api.set_medium(self.handle)
            reduced = self._api.describe(self.handle); validate_restricted(before, reduced)
            self.receipt['derivedBefore'] = reduced
            # Only this private derived token receives the narrowly validated
            # current-user ACE. No process exists yet, and a failed readback
            # prevents readiness/creation rather than falling back to High IL.
            self.receipt['defaultDacl'] = default_dacl.ensure_derived_default_dacl(
                self._api, source, self.handle, before, reduced,
                source_default_before=self._source_default_before)
            self.receipt['ready'] = True
        except BaseException as error:
            if isinstance(error, default_dacl.DefaultDaclError) and error.receipt is not None:
                self.receipt['defaultDacl'] = error.receipt
            original = error
        finally:
            if source:
                try: self._api.close(source)
                except BaseException as error:
                    if original is None: original = error
                    elif hasattr(original, 'add_note'): original.add_note('source-token-close:' + str(error))
        if original is not None:
            self.close(original)
            raise original
        return self

    def create_suspended(self, executable: str, command: Any, flags: int, environment: Any,
                         directory: str, startup: Any, info: Any) -> None:
        require(self.receipt['ready'] and self.handle and not self._created, 'token not ready or already used')
        require(type(flags) is int and flags == BASE_FLAGS | CREATE_SUSPENDED, 'exact safe suspended creation flags')
        require(startup.lpAttributeList and startup.StartupInfo.cb == ctypes.sizeof(startup), 'STARTUPINFOEX attribute list required')
        require(type(startup.StartupInfo.lpDesktop) is str and startup.StartupInfo.lpDesktop.startswith('WinSta0\\')
                and startup.StartupInfo.lpDesktop.split('\\')[-1].casefold() != 'default', 'explicit non-input lab Desktop required')
        require(startup.StartupInfo.dwFlags == 0x100, 'explicit standard-handle whitelist required')
        require(all((startup.StartupInfo.hStdInput, startup.StartupInfo.hStdOutput, startup.StartupInfo.hStdError)),
                'all standard handles must be explicit')
        self._api.create(self.handle, executable, command, flags, environment, directory, startup, info)
        # No fallible post-create work here: caller immediately adopts both
        # PROCESS_INFORMATION handles before verifying the suspended token.
        self._created = True

    def verify_child(self, process: int) -> None:
        require(self._created and self.handle and process, 'no exact child process authority')
        token = self._api.open_process(process)
        try:
            observed = self._api.describe(token)
            actual_default = default_dacl.inspect_default_security(self._api, token, identity=observed)
        finally: self._api.close(token)
        validate_observation(observed)
        self.receipt['childBeforeResume'] = observed
        self.receipt['childDefaultBeforeResume'] = default_dacl.default_security_summary(actual_default)
        require(security_profile(observed) == security_profile(self.receipt['derivedBefore']),
                'actual suspended child token differs from requested restricted token')
        default_dacl.assert_child_default_security(actual_default, self.receipt['defaultDacl'])
        self.receipt['childTokenVerifiedBeforeResume'] = True

    def close(self, original: BaseException | None = None) -> None:
        errors = []
        if self._api is None: return
        if 'sourceBefore' in self.receipt and 'sourceAfter' not in self.receipt:
            source = 0
            try:
                source = self._api.open_current(); after = self._api.describe(source); validate_observation(after)
                self.receipt['sourceAfter'] = after
                before = self.receipt['sourceBefore']
                require(after['tokenId'] == before['tokenId'] and security_profile(after) == security_profile(before),
                        'caller token security state changed')
                actual_default = default_dacl.inspect_default_security(self._api, source, identity=after)
                self.receipt['sourceDefaultAfter'] = default_dacl.default_security_summary(actual_default)
                require(self._source_default_before is not None, 'source default-security baseline unavailable')
                default_dacl.assert_source_default_security(actual_default,
                    dict(sourceBefore=self.receipt['sourceDefaultBefore']))
                self.receipt['parentTokenUnchanged'] = True
            except BaseException as error: errors.append('source-token-check:' + str(error))
            finally:
                if source:
                    try: self._api.close(source)
                    except BaseException as error: errors.append('source-token-close:' + str(error))
        if self.handle:
            try:
                self._api.close(self.handle); self.handle = 0; self.receipt['derivedTokenClosed'] = True
            except BaseException as error: errors.append('derived-token-close:' + str(error))
        self.receipt['cleanupErrors'].extend(errors)
        if errors:
            if original is not None and hasattr(original, 'add_note'): original.add_note('; '.join(errors))
            elif original is None: raise TokenSafetyError('; '.join(errors))

    def __exit__(self, kind: Any, original: BaseException | None, traceback: Any) -> bool:
        self.close(original); return False
