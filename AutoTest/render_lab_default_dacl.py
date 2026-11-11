"""Narrow default-object DACL repair for ONE private derived lab token.

No native work happens at import. The caller must already own distinct source
and restricted Medium primary-token handles. Only the private derived token's
TokenDefaultDacl (class 6) may be set; no existing object ACL, caller token,
owner/group, group attributes, privilege, impersonation state, or UAC setting
is modified. This does not turn the render lab into a general security sandbox.

Call ensure_derived_default_dacl after deriving/validating Medium and BEFORE
creating any process. A failed operation raises DefaultDaclError with a receipt;
the token owner must close the private token and must not launch a child.
The caller should use assert_source_default_security again at its final close
and assert_child_default_security on the actual child token before resuming it.
"""
from __future__ import annotations

import copy
import ctypes as ct
from ctypes import wintypes as wt
from dataclasses import dataclass
import hashlib
import re
import struct
from typing import Any


GENERIC_ALL = 0x10000000
MEDIUM_RID = 8192
ACL_LIMIT = 65535  # ACL.AclSize and ACE.AceSize are USHORT, not DWORD.
ADMIN_SID = 'S-1-5-32-544'


class DefaultDaclError(RuntimeError):
    def __init__(self, message: str, receipt: dict[str, Any] | None = None):
        super().__init__(message)
        self.receipt = receipt


def require(value: Any, message: str) -> None:
    if not value:
        raise DefaultDaclError(message)


def encode_sid(text: str) -> bytes:
    require(type(text) is str and re.fullmatch(r'S-1-\d+(?:-\d+){0,15}', text), 'canonical SID text')
    values = [int(part) for part in text[2:].split('-')]
    revision, authority, *sub = values
    require(authority < 1 << 48 and all(item < 1 << 32 for item in sub), 'SID numeric bounds')
    require(text == 'S-' + '-'.join(map(str, values)), 'noncanonical SID text')
    return bytes((revision, len(sub))) + authority.to_bytes(6, 'big') + struct.pack('<' + 'I' * len(sub), *sub)


def decode_sid(data: bytes, offset: int, end: int) -> tuple[str, int]:
    require(type(data) is bytes and 0 <= offset <= end - 8 <= len(data) - 8, 'SID header bounds')
    revision, count = data[offset:offset + 2]
    require(revision == 1 and count <= 15, 'SID revision/count')
    length = 8 + count * 4
    require(offset + length <= end, 'SID extent bounds')
    authority = int.from_bytes(data[offset + 2:offset + 8], 'big')
    sub = struct.unpack_from('<' + 'I' * count, data, offset + 8)
    return 'S-' + '-'.join(map(str, (revision, authority, *sub))), length


@dataclass(frozen=True)
class AllowAce:
    mask: int
    sid: str
    raw: bytes


@dataclass(frozen=True)
class SimpleAcl:
    revision: int
    entries: tuple[AllowAce, ...]
    padding: bytes


@dataclass(frozen=True)
class DaclPlan:
    before: bytes
    after: bytes
    user_sid: str
    changed: bool


def parse_simple_acl(data: bytes | None) -> SimpleAcl:
    """Fail closed for null, deny, inherited, object/callback, or unknown ACEs."""
    require(type(data) is bytes and 8 <= len(data) <= ACL_LIMIT, 'non-null bounded ACL required')
    revision, reserved1, size, count, reserved2 = struct.unpack_from('<BBHHH', data)
    require(revision in (2, 4) and reserved1 == 0 and reserved2 == 0, 'ACL revision/reserved fields')
    require(size == len(data) and size % 4 == 0, 'ACL exact size/alignment')
    cursor = 8
    entries = []
    for _ in range(count):
        require(cursor <= size - 4, 'ACE header bounds')
        kind, flags, length = struct.unpack_from('<BBH', data, cursor)
        require(kind == 0 and flags == 0, 'only explicit noninheriting allow ACEs supported')
        require(length >= 16 and length % 4 == 0 and cursor + length <= size, 'ACE extent/alignment')
        mask = struct.unpack_from('<I', data, cursor + 4)[0]
        sid, sid_length = decode_sid(data, cursor + 8, cursor + length)
        require(length == 8 + sid_length, 'opaque/trailing ACE payload refused')
        entries.append(AllowAce(mask, sid, data[cursor:cursor + length]))
        cursor += length
    return SimpleAcl(revision, tuple(entries), data[cursor:])


def plan_user_object_dacl(data: bytes | None, user_sid: str) -> DaclPlan:
    parsed = parse_simple_acl(data)
    sid = encode_sid(user_sid)
    if any(entry.sid == user_sid and entry.mask & GENERIC_ALL for entry in parsed.entries):
        return DaclPlan(data, data, user_sid, False)
    added = struct.pack('<BBHI', 0, 0, 8 + len(sid), GENERIC_ALL) + sid
    length = len(data) + len(added)
    require(length <= ACL_LIMIT and len(parsed.entries) < 65535, 'expanded ACL bound')
    # All existing ACEs AND existing unused bytes are preserved. Only header
    # size/count changes; the one explicit current-user ACE precedes old slack.
    after = struct.pack('<BBHHH', parsed.revision, 0, length, len(parsed.entries) + 1, 0)
    after += b''.join(entry.raw for entry in parsed.entries) + added + parsed.padding
    check = parse_simple_acl(after)
    require(check.entries[:-1] == parsed.entries and check.padding == parsed.padding,
            'existing ACL entries/padding changed')
    return DaclPlan(data, after, user_sid, True)


def acl_summary(data: bytes | None) -> dict[str, Any]:
    if data is None:
        return dict(null=True)
    result = dict(null=False, bytes=len(data), sha256=hashlib.sha256(data).hexdigest().upper())
    try:
        parsed = parse_simple_acl(data)
        result.update(revision=parsed.revision, aceCount=len(parsed.entries),
            entries=[dict(type=0, flags=0, mask=entry.mask, sid=entry.sid) for entry in parsed.entries],
            unusedBytes=len(parsed.padding))
    except DefaultDaclError as error:
        result['unsupportedReason'] = str(error)
    return result


def _stable_identity(row: dict[str, Any]) -> dict[str, Any]:
    # ModifiedId is accounting/version metadata, not a permission. Setting the
    # private DefaultDacl is expected to change that private token's ModifiedId.
    return {key: copy.deepcopy(value) for key, value in row.items() if key != 'modifiedId'}


def _validate_security(row: dict[str, Any]) -> None:
    require(type(row) is dict and set(row) == {'identity', 'ownerSid', 'primaryGroupSid', 'defaultDacl'},
            'default-security exact schema')
    require(type(row['identity']) is dict, 'token identity required')
    for field in ('ownerSid', 'primaryGroupSid'):
        encode_sid(row[field])
    require(row['defaultDacl'] is None or type(row['defaultDacl']) is bytes, 'DACL bytes required')


def _public_security(row: dict[str, Any]) -> dict[str, Any]:
    return dict(identity=copy.deepcopy(row['identity']), ownerSid=row['ownerSid'],
                primaryGroupSid=row['primaryGroupSid'], defaultDacl=acl_summary(row['defaultDacl']))


def default_security_summary(row: dict[str, Any]) -> dict[str, Any]:
    """JSON-safe receipt; callers keep the independently queried raw baseline."""
    _validate_security(row)
    return _public_security(row)


def _same_default_security(before: dict[str, Any], after: dict[str, Any]) -> bool:
    return (_stable_identity(before['identity']) == _stable_identity(after['identity'])
            and all(before[field] == after[field] for field in ('ownerSid', 'primaryGroupSid', 'defaultDacl')))


class _TokenDefaultDacl(ct.Structure):
    _fields_ = [('DefaultDacl', wt.LPVOID)]


def _pointer_payload(buffer: Any, minimum: int) -> tuple[bytes, int | None]:
    header = ct.sizeof(wt.LPVOID)
    require(header <= ct.sizeof(buffer) <= 65536, 'token returned-buffer bound')
    pointer = wt.LPVOID.from_buffer_copy(buffer).value
    if not pointer:
        return bytes(buffer), None
    offset = pointer - ct.addressof(buffer)
    require(header <= offset <= ct.sizeof(buffer) - minimum, 'token payload pointer bounds')
    return bytes(buffer), offset


def inspect_default_security(token_api: Any, token: int,
                             *, identity: dict[str, Any] | None = None) -> dict[str, Any]:
    """Read only: permits a freshly opened source or actual suspended child.

    An identity argument must be the caller's fresh describe of this same open
    handle. The guarded writer never accepts that shortcut: it always performs
    a fresh describe itself immediately before its only class-6 write.
    """
    require(type(token) is int and token > 0, 'open token handle required')
    row = dict(identity=copy.deepcopy(token_api.describe(token) if identity is None else identity))
    for field, kind in (('ownerSid', 4), ('primaryGroupSid', 5), ('defaultDacl', 6)):
        data, offset = _pointer_payload(token_api._query(token, kind), 8)
        if kind == 6:
            if offset is None:
                row[field] = None
            else:
                size = struct.unpack_from('<H', data, offset + 2)[0]
                require(8 <= size <= len(data) - offset, 'default DACL native extent')
                row[field] = data[offset:offset + size]
        else:
            require(offset is not None, field + ' missing')
            row[field] = decode_sid(data, offset, len(data))[0]
    _validate_security(row)
    return row


class NativeDefaultDaclApi:
    """Borrow the token owner's API/handles; no native calls in construction."""
    def __init__(self, token_api: Any, source: int, derived: int,
                 source_identity: dict[str, Any], derived_identity: dict[str, Any]):
        require(type(source) is int and type(derived) is int and source > 0 and derived > 0 and source != derived,
                'distinct owned source/derived handles required')
        self.api, self.source, self.derived = token_api, source, derived
        self.source_identity = copy.deepcopy(source_identity)
        self.derived_identity = copy.deepcopy(derived_identity)

    def inspect(self, token: int) -> dict[str, Any]:
        require(token in (self.source, self.derived), 'unowned token query refused')
        return inspect_default_security(self.api, token)

    def set_derived_default_dacl(self, token: int, expected_before: bytes, new_acl: bytes, user_sid: str) -> None:
        # Defense in depth: even a mistaken caller cannot route this method to
        # the source, swap tokens, change the user, or supply an arbitrary ACL.
        require(token == self.derived and token != self.source, 'only bound derived handle may be written')
        current = self.inspect(token)
        require(_stable_identity(current['identity']) == _stable_identity(self.derived_identity),
                'derived token identity/security drift before DACL write')
        require(current['identity']['tokenId'] != self.source_identity['tokenId'], 'source token alias refused')
        require(current['identity']['userSid'] == self.source_identity['userSid'] == user_sid,
                'DACL SID must equal both actual TokenUser identities')
        require(current['defaultDacl'] == expected_before, 'derived DACL changed before write')
        plan = plan_user_object_dacl(expected_before, user_sid)
        require(plan.changed and plan.after == new_acl, 'only the exact append-current-user plan may be written')
        buffer = ct.create_string_buffer(new_acl, len(new_acl))
        value = _TokenDefaultDacl(ct.cast(buffer, wt.LPVOID))
        self.api.check(self.api.a.SetTokenInformation(token, 6, ct.byref(value), ct.sizeof(value)),
                       'SetTokenInformation(private derived DefaultDacl only)')


def ensure_derived_default_dacl(token_api: Any, source: int, derived: int,
                                source_observation: dict[str, Any], derived_observation: dict[str, Any],
                                *, source_default_before: dict[str, Any] | None = None,
                                _api: Any = None) -> dict[str, Any]:
    """Append only the actual current user; verify real before/after, or fail."""
    require(type(source) is int and type(derived) is int and source > 0 and derived > 0 and source != derived,
            'distinct source/derived handles required')
    api = _api if _api is not None else NativeDefaultDaclApi(
        token_api, source, derived, source_observation, derived_observation)
    receipt: dict[str, Any] = dict(schema=1, mode='private-derived-default-object-dacl/v1',
        ok=False, changed=False, classWritten=None, sourceUnchanged=False,
        derivedReadbackVerified=False, sourceTokenWritten=False, sharedObjectAclChanged=False,
        impersonationPerformed=False, errors=[])
    before_source = None
    original: BaseException | None = None
    try:
        actual_source = copy.deepcopy(api.inspect(source)); _validate_security(actual_source)
        before_source = copy.deepcopy(actual_source if source_default_before is None else source_default_before)
        _validate_security(before_source)
        receipt['sourceBefore'] = _public_security(before_source)
        receipt['sourceAtDaclPreflight'] = _public_security(actual_source)
        require(_same_default_security(before_source, actual_source),
                'source default security changed since pre-derive read')
        before_derived = copy.deepcopy(api.inspect(derived)); _validate_security(before_derived)
        receipt['derivedBefore'] = _public_security(before_derived)
        a, b = before_source['identity'], before_derived['identity']
        require(_stable_identity(a) == _stable_identity(source_observation), 'source observation drift')
        require(_stable_identity(b) == _stable_identity(derived_observation), 'derived observation drift')
        require(a['tokenType'] == b['tokenType'] == 1 and a['tokenId'] != b['tokenId'], 'distinct primary tokens required')
        for field in ('userSid', 'authenticationId', 'sessionId'):
            require(a[field] == b[field], 'derived identity mismatch: ' + field)
        require(b['integrityRid'] == MEDIUM_RID and b['hasRestrictions'] is True,
                'already-restricted Medium token required')
        require(b['handleInheritable'] is False and a['handleInheritable'] is False, 'noninheritable tokens required')
        require(b['uiAccess'] is False and b['appContainer'] is False and b['sandboxInert'] is False,
                'restricted token profile required')
        require(all(p['name'] == 'SeChangeNotifyPrivilege' for p in b['privileges']), 'restricted privilege profile required')
        require(all(g['sid'] != ADMIN_SID or g['attributes'] & 0x10 and not g['attributes'] & 0x6
                    for g in b['groups']), 'Administrators must stay deny-only')
        require(before_derived['ownerSid'] == b['userSid'], 'derived owner must already be current user; no owner repair')
        parse_simple_acl(before_source['defaultDacl'])
        plan = plan_user_object_dacl(before_derived['defaultDacl'], b['userSid'])
        receipt['expectedAfterDacl'] = acl_summary(plan.after)
        if plan.changed:
            receipt['classWritten'] = 6
            api.set_derived_default_dacl(derived, plan.before, plan.after, plan.user_sid)
            receipt['changed'] = True
        after_derived = copy.deepcopy(api.inspect(derived)); _validate_security(after_derived)
        receipt['derivedAfter'] = _public_security(after_derived)
        require(_stable_identity(after_derived['identity']) == _stable_identity(b), 'derived security changed beyond DefaultDacl')
        require(all(after_derived[field] == before_derived[field] for field in ('ownerSid', 'primaryGroupSid')),
                'derived owner/group changed')
        require(after_derived['defaultDacl'] == plan.after, 'derived DefaultDacl readback differs from exact plan')
        receipt['derivedReadbackVerified'] = True
    except BaseException as error:
        original = error
        receipt['errors'].append(f'{type(error).__name__}: {error}')
    finally:
        if before_source is not None:
            try:
                after_source = copy.deepcopy(api.inspect(source)); _validate_security(after_source)
                receipt['sourceAfter'] = _public_security(after_source)
                require(_same_default_security(before_source, after_source), 'source token Owner/Group/DefaultDacl/security changed')
                receipt['sourceUnchanged'] = True
            except BaseException as error:
                receipt['errors'].append(f'source-after: {type(error).__name__}: {error}')
                if original is None:
                    original = error
    receipt['ok'] = receipt['sourceUnchanged'] and receipt['derivedReadbackVerified'] and not receipt['errors']
    if not receipt['ok']:
        raise DefaultDaclError('; '.join(receipt['errors']), receipt) from original
    return receipt


def assert_source_default_security(actual: dict[str, Any], receipt: dict[str, Any]) -> None:
    """Call on a fresh final source-token read, not an earlier cached snapshot."""
    _validate_security(actual)
    expected = receipt['sourceBefore']
    observed = _public_security(actual)
    require(_stable_identity(observed['identity']) == _stable_identity(expected['identity'])
            and all(observed[field] == expected[field] for field in ('ownerSid', 'primaryGroupSid', 'defaultDacl')),
            'final source default security changed')


def assert_child_default_security(actual: dict[str, Any], receipt: dict[str, Any]) -> None:
    """Only default-object security here; the token owner checks full child identity."""
    _validate_security(actual)
    require(receipt.get('ok') is True and receipt.get('derivedReadbackVerified') is True,
            'verified derived DACL receipt required before child check')
    expected = receipt['derivedAfter']
    observed = _public_security(actual)
    require(observed['identity']['userSid'] == expected['identity']['userSid'], 'child default-security user mismatch')
    require(all(observed[field] == expected[field] for field in ('ownerSid', 'primaryGroupSid', 'defaultDacl')),
            'actual child Owner/Group/DefaultDacl differs from verified derived token')
