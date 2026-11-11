"""Read-only security diagnosis for the restricted render-lab launch.

The existing token owner derives a PRIVATE Medium token, then this file only
queries it and the caller's token. No caller-token/ACL changes, impersonation,
process launch, GPU work, or global-security writes. DefaultDacl/Owner/Group
are always inspected; --token-only does not even create a disposable Desktop.
Optional AccessCheck duplicates a token HANDLE, never installs it on a thread.
This is diagnostic evidence, not proof that a DLL will initialize successfully.
"""
import argparse
import ctypes as ct
from ctypes import wintypes as wt
import hashlib
import json
import struct
import sys
from isolated_render_desktop import IsolatedRenderDesktop
from render_lab_token import RestrictedMediumToken


BOUND = 65536
ALL_SECURITY = 1 | 2 | 4 | 16  # owner, group, DACL, LABEL; NOT full SACL.
MAPPINGS = {'station': (0x20303, 0x2001c, 0x20060, 0xf037f),
            'desktop': (0x20041, 0x200be, 0x20100, 0xf01ff)}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def cleanup(action, name):
    """An ancillary cleanup failure must not hide the original query failure."""
    original = sys.exception()
    try:
        action()
    except BaseException as error:
        if original is None:
            raise
        original.add_note(f'{name}: {type(error).__name__}: {error}')


def error_text(error):
    notes = getattr(error, '__notes__', ())
    return f'{type(error).__name__}: {error}' + ('; ' + '; '.join(notes) if notes else '')


def sid_at(data, offset, limit=None):
    """Decode only a bounded, complete native SID; no native function calls."""
    end = len(data) if limit is None else min(len(data), limit)
    require(0 <= offset <= end - 8, 'SID header bounds')
    revision, count = data[offset:offset + 2]
    require(revision == 1 and count <= 15, 'SID revision/subauthority count')
    require(offset + 8 + count * 4 <= end, 'SID subauthority bounds')
    authority = int.from_bytes(data[offset + 2:offset + 8], 'big')
    parts = struct.unpack_from('<' + 'I' * count, data, offset + 8)
    return 'S-' + '-'.join(map(str, (revision, authority, *parts)))


def acl_at(data, offset):
    """Keep unfamiliar ACEs explicit; never silently classify them as grants."""
    require(0 <= offset <= len(data) - 8, 'ACL header bounds')
    revision, _, size, count, _ = struct.unpack_from('<BBHHH', data, offset)
    require(revision in (2, 4) and 8 <= size <= len(data) - offset, 'ACL revision/size')
    end = offset + size
    cursor = offset + 8
    entries = []
    for index in range(count):
        require(cursor <= end - 4, 'ACE header bounds')
        kind, flags, length = struct.unpack_from('<BBH', data, cursor)
        require(length >= 4 and length % 4 == 0 and cursor + length <= end, 'ACE size bounds')
        entry = dict(index=index, type=kind, flags=flags, bytes=length)
        if kind in (0, 1, 2, 3, 0x11):  # basic allow/deny/audit/alarm/mandatory-label
            require(length >= 16, 'basic ACE too short')
            entry.update(mask=struct.unpack_from('<I', data, cursor + 4)[0],
                         sid=sid_at(data, cursor + 8, cursor + length))
        else:
            entry.update(unparsedType=True,
                         sha256=hashlib.sha256(data[cursor:cursor + length]).hexdigest())
        entries.append(entry)
        cursor += length
    return dict(null=False, revision=revision, bytes=size, aceCount=count,
                trailingAclBytes=end - cursor,
                sha256=hashlib.sha256(data[offset:end]).hexdigest(), entries=entries)


def _pointed_offset(buffer, header_bytes):
    require(ct.sizeof(buffer) >= header_bytes, 'token pointer header bounds')
    pointer = wt.LPVOID.from_buffer_copy(buffer).value
    if not pointer:
        return None
    offset = pointer - ct.addressof(buffer)
    require(header_bytes <= offset < ct.sizeof(buffer), 'token pointer outside returned buffer')
    return offset


def token_security(api, token):
    result = {}
    for name, kind in (('owner', 4), ('primaryGroup', 5), ('defaultDacl', 6)):
        buffer = api._query(token, kind)
        offset = _pointed_offset(buffer, ct.sizeof(wt.LPVOID))
        data = bytes(buffer)
        if kind == 6:
            result[name] = dict(null=True) if offset is None else acl_at(data, offset)
        else:
            require(offset is not None, name + ' SID missing')
            result[name] = sid_at(data, offset)
    return result


def descriptor_layout(data):
    require(len(data) >= 20, 'security descriptor header bounds')
    revision, _, control, owner, group, sacl, dacl = struct.unpack_from('<BBHIIII', data)
    require(revision == 1 and control & 0x8000, 'self-relative security descriptor required')
    require(all(offset == 0 or offset >= 20 for offset in (owner, group)),
            'owner/group overlaps descriptor header')
    result = dict(control=control, owner=sid_at(data, owner) if owner else None,
                  group=sid_at(data, group) if group else None,
                  daclPresent=bool(control & 4), saclPresent=bool(control & 16))
    for name, offset in (('dacl', dacl), ('sacl', sacl)):
        require(offset == 0 or offset >= 20, name + ' overlaps descriptor header')
        result[name] = acl_at(data, offset) if offset else dict(null=True)
    return result


class _GenericMapping(ct.Structure):
    _fields_ = [(name, wt.DWORD) for name in ('read', 'write', 'execute', 'all')]


class _SecurityApi:
    """Native bindings are initialized only when the explicitly run main uses them."""
    def __init__(self):
        self.u = ct.WinDLL('user32', use_last_error=True)
        self.a = ct.WinDLL('advapi32', use_last_error=True)
        self.k = ct.WinDLL('kernel32', use_last_error=True)
        for library, name, result, args in (
            (self.u, 'GetUserObjectSecurity', wt.BOOL,
             (wt.HANDLE, ct.POINTER(wt.DWORD), wt.LPVOID, wt.DWORD, ct.POINTER(wt.DWORD))),
            (self.a, 'ConvertSecurityDescriptorToStringSecurityDescriptorW', wt.BOOL,
             (wt.LPVOID, wt.DWORD, wt.DWORD, ct.POINTER(wt.LPWSTR), ct.POINTER(wt.DWORD))),
            (self.a, 'GetSecurityInfo', wt.DWORD,
             (wt.HANDLE, ct.c_int, wt.DWORD, wt.LPVOID, wt.LPVOID, wt.LPVOID, wt.LPVOID, ct.POINTER(wt.LPVOID))),
            (self.a, 'GetSecurityDescriptorLength', wt.DWORD, (wt.LPVOID,)),
            (self.a, 'DuplicateTokenEx', wt.BOOL,
             (wt.HANDLE, wt.DWORD, wt.LPVOID, ct.c_int, ct.c_int, ct.POINTER(wt.HANDLE))),
            (self.a, 'AccessCheck', wt.BOOL,
             (wt.LPVOID, wt.HANDLE, wt.DWORD, ct.POINTER(_GenericMapping), wt.LPVOID,
              ct.POINTER(wt.DWORD), ct.POINTER(wt.DWORD), ct.POINTER(wt.BOOL))),
            (self.k, 'CloseHandle', wt.BOOL, (wt.HANDLE,)),
            (self.k, 'LocalFree', wt.HANDLE, (wt.HANDLE,)),
        ):
            function = getattr(library, name)
            function.argtypes, function.restype = args, result

    @staticmethod
    def check(value, operation):
        if not value:
            raise OSError(ct.get_last_error(), operation)
        return value

    def user_descriptor(self, handle, flags):
        info, size = wt.DWORD(flags), wt.DWORD()
        self.u.GetUserObjectSecurity(handle, ct.byref(info), None, 0, ct.byref(size))
        require(0 < size.value <= BOUND, 'bounded user-object descriptor')
        data = ct.create_string_buffer(size.value)
        self.check(self.u.GetUserObjectSecurity(handle, ct.byref(info), data,
                   len(data), ct.byref(size)), 'GetUserObjectSecurity')
        require(0 < size.value <= len(data), 'returned user-object descriptor length')
        return bytes(data)[:size.value]

    def security_info(self, handle):
        pointer = wt.LPVOID()
        error = self.a.GetSecurityInfo(handle, 7, ALL_SECURITY, None, None, None, None, ct.byref(pointer))
        if error:
            raise OSError(error, 'GetSecurityInfo(SE_WINDOW_OBJECT)')
        self.check(pointer, 'GetSecurityInfo returned no descriptor')
        try:
            size = self.a.GetSecurityDescriptorLength(pointer)
            require(20 <= size <= BOUND, 'bounded GetSecurityInfo descriptor')
            return ct.string_at(pointer, size)
        finally:
            cleanup(lambda: self.free(pointer), 'LocalFree(GetSecurityInfo)')

    def free(self, pointer):
        if self.k.LocalFree(ct.cast(pointer, wt.LPVOID)):
            raise OSError(ct.get_last_error(), 'LocalFree')

    def describe(self, data, flags):
        buffer = ct.create_string_buffer(data)
        text = wt.LPWSTR()
        self.check(self.a.ConvertSecurityDescriptorToStringSecurityDescriptorW(
            buffer, 1, flags, ct.byref(text), None), 'ConvertSecurityDescriptorToStringSecurityDescriptorW')
        try:
            return dict(sddl=text.value, bytes=len(data), sha256=hashlib.sha256(data).hexdigest(),
                        layout=descriptor_layout(data))
        finally:
            cleanup(lambda: self.free(text), 'LocalFree(SDDL)')

    def access_check(self, data, primary_token, mapping_values):
        layout = descriptor_layout(data)
        require(layout['owner'] is not None and layout['group'] is not None,
                'AccessCheck requires owner and group')
        duplicate = wt.HANDLE()
        # A query-only impersonation-token HANDLE, never assigned to a thread.
        self.check(self.a.DuplicateTokenEx(primary_token, 0x8, None, 2, 2, ct.byref(duplicate)),
                   'DuplicateTokenEx(query-only AccessCheck)')
        try:
            buffer = ct.create_string_buffer(data)
            mapping = _GenericMapping(*mapping_values)
            rows = []
            for name, desired in (('maximumAllowed', 0x02000000), ('all', mapping_values[3])):
                privilege_bytes = wt.DWORD(1024)
                privileges = ct.create_string_buffer(privilege_bytes.value)
                granted, allowed = wt.DWORD(), wt.BOOL()
                success = self.a.AccessCheck(buffer, duplicate, desired, ct.byref(mapping), privileges,
                    ct.byref(privilege_bytes), ct.byref(granted), ct.byref(allowed))
                error = ct.get_last_error()
                if not success and error == 122:
                    require(1024 < privilege_bytes.value <= BOUND, 'bounded AccessCheck privilege set')
                    privileges = ct.create_string_buffer(privilege_bytes.value)
                    success = self.a.AccessCheck(buffer, duplicate, desired, ct.byref(mapping), privileges,
                        ct.byref(privilege_bytes), ct.byref(granted), ct.byref(allowed))
                    error = ct.get_last_error()
                row = dict(name=name, requested=desired, apiSucceeded=bool(success))
                if success:
                    require(8 <= privilege_bytes.value <= len(privileges), 'AccessCheck privilege-set length')
                    count = wt.DWORD.from_buffer_copy(privileges).value
                    require(count <= (privilege_bytes.value - 8) // 12, 'AccessCheck privilege count')
                    row.update(accessAllowed=bool(allowed.value), granted=int(granted.value),
                               usedPrivilegeCount=int(count))
                    if not allowed.value:
                        row['accessError'] = error
                else:
                    row['apiError'] = error
                rows.append(row)
            return dict(impersonationPerformed=False, tokenHandleOnly=True,
                        interpretation='descriptor-access-only-not-DLL-initialization-proof', checks=rows)
        finally:
            cleanup(lambda: self.check(self.k.CloseHandle(duplicate),
                                       'CloseHandle(AccessCheck token)'), 'AccessCheck-token')


def inspect(handle, *, api=None, token=None, cross_security_info=False, access_check=False, kind='desktop'):
    api = api or _SecurityApi()
    result = {}
    for name, flags in (('dacl', 4), ('integrityLabel', 16)):
        try:
            result[name] = api.describe(api.user_descriptor(handle, flags), flags)
        except Exception as error:
            result[name] = dict(error=error_text(error))
    if cross_security_info or access_check:
        try:
            full = api.user_descriptor(handle, ALL_SECURITY)
            result['ownerGroupDaclLabel'] = api.describe(full, ALL_SECURITY)
            if access_check:
                result['accessCheck'] = api.access_check(full, token, MAPPINGS[kind])
        except Exception as error:
            result['fullDescriptorError'] = error_text(error)
    if cross_security_info:
        try:
            result['getSecurityInfo'] = api.describe(api.security_info(handle), ALL_SECURITY)
        except Exception as error:
            result['getSecurityInfo'] = dict(error=error_text(error))
    return result


def query_errors(value, path=''):
    """Partial query failures stay visible and cannot produce queriesComplete."""
    found = []
    if isinstance(value, dict):
        for key, item in value.items():
            field = path + '.' + key if path else key
            # AccessStatus=false is a valid denial observation, not an API failure.
            if key == 'error' or key.endswith('Error') and key != 'accessError':
                found.append(dict(path=field, error=item))
            elif key == 'apiSucceeded' and item is False:
                found.append(dict(path=field, error='API failed'))
            else:
                found.extend(query_errors(item, field))
    elif isinstance(value, list):
        for index, item in enumerate(value):
            found.extend(query_errors(item, f'{path}[{index}]'))
    return found


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--token-only', action='store_true')
    parser.add_argument('--cross-security-info', action='store_true')
    parser.add_argument('--access-check', action='store_true')
    args = parser.parse_args()
    if args.token_only and (args.cross_security_info or args.access_check):
        parser.error('--token-only cannot be combined with Desktop queries')
    result = dict(schema=2, scope='READ_ONLY_RENDER_LAB_SECURITY_DIAGNOSTIC',
        sharedSecurityChanged=False, callerTokenUnchanged=None, processLaunched=False,
        impersonationPerformed=False, privateDerivedTokenCreated=False, queriesComplete=False)
    desktop = None
    reduced = RestrictedMediumToken()
    result['tokenLifecycle'] = reduced.receipt
    try:
        with reduced:
            result['privateDerivedTokenCreated'] = True
            source = reduced._api.open_process(reduced._api.k.GetCurrentProcess(), 0x8)
            try:
                result['sourceTokenSecurity'] = token_security(reduced._api, source)
            finally:
                cleanup(lambda: reduced._api.close(source), 'source-token-close')
            result['derivedTokenSecurity'] = token_security(reduced._api, reduced.handle)
            if not args.token_only:
                api = _SecurityApi()
                with IsolatedRenderDesktop() as desktop:
                    result['desktopSafety'] = desktop.receipt
                    u = desktop._api.u
                    u.OpenDesktopW.argtypes = [wt.LPCWSTR, wt.DWORD, wt.BOOL, wt.DWORD]
                    u.OpenDesktopW.restype = wt.HANDLE
                    handle = u.OpenDesktopW(desktop.desktop_name, 0, False, 0x20000)
                    if not handle:
                        raise OSError(ct.get_last_error(), 'OpenDesktopW(owned READ_CONTROL)')
                    try:
                        options = dict(api=api, token=reduced.handle,
                            cross_security_info=args.cross_security_info, access_check=args.access_check)
                        result.update(inputDesktop=desktop.input_before, desktopName=desktop.desktop_name,
                            station=inspect(u.GetProcessWindowStation(), kind='station', **options),
                            newDesktop=inspect(handle, **options))
                    finally:
                        cleanup(lambda: api.check(u.CloseDesktop(handle), 'CloseDesktop(query)'),
                                'Desktop-query-close')
        result['queryErrors'] = query_errors({key: result[key] for key in ('station', 'newDesktop') if key in result})
        result['queriesComplete'] = not result['queryErrors']
    except Exception as error:
        result['error'] = error_text(error)
    finally:
        result['callerTokenUnchanged'] = reduced.receipt.get('parentTokenUnchanged', False)
        if desktop is not None:
            result['desktopClosed'] = desktop.receipt['desktopClosed']
            result['cleanupErrors'] = desktop.receipt['cleanupErrors']
    print(json.dumps(result))
    sys.exit(0 if result['queriesComplete'] and result['callerTokenUnchanged']
             and reduced.receipt['derivedTokenClosed'] and not reduced.receipt['cleanupErrors']
             and (desktop is None or result['desktopClosed'] and not result['cleanupErrors']) else 1)
