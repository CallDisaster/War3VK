"""Pure source-handoff gate for the formal dev-host HWND adapter.

This deliberately freezes a small set of executable C++ token shapes. It is
not a C++ compiler, concurrency proof, device-loss injection, GPU/VVL test, or
proof of HWND pixels. Comments/whitespace cannot satisfy a missing handoff.
Counterexamples mutate in-memory copies of the actual formal sources only;
this script never patches files, builds, launches, or imports artifact code.
Idle transport wake/cancellation remains a separate product-owner boundary.
"""
from dataclasses import dataclass
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
CORE = ROOT / 'tools/render_host/dxvk_core'
SOURCE_FILES = {
    'backend': CORE / 'd3d9_backend.cpp',
    'header': CORE / 'd3d9_backend.h',
    'host': CORE / 'd3d9_host_main.cpp',
    'target': CORE / 'host_surface_target.cpp',
}

# Quoted literals precede comments so e.g. "https://" remains one token.
# This restricted lexer fails closed on raw literals rather than pretending
# to parse arbitrary future C++ syntax.
LEXER = re.compile(
    r'(?:u8|u|U|L)?"(?:\\.|[^"\\])*"'
    r"|(?:u8|u|U|L)?'(?:\\.|[^'\\])*'"
    r'|//[^\r\n]*|/\*.*?\*/'
    r'|::|->|==|!=|<=|>=|&&|\|\||\+\+|--|\.\.\.'
    r"|[A-Za-z_][A-Za-z_0-9]*|[0-9][0-9A-Za-z_'.]*|[^\s]", re.DOTALL)


class ContractViolation(AssertionError):
    pass


def need(condition, message):
    if not condition:
        raise ContractViolation(message)


def lex(text):
    result = []
    for match in LEXER.finditer(text):
        value = match.group()
        if value.startswith(('//', '/*')):
            continue
        need(not (value in ('R', 'u8R', 'uR', 'UR', 'LR') and text[match.end():].startswith('"')),
             'raw C++ literal requires lexer review')
        result.append((value, match.start(), match.end()))
    return result


def tokens(text):
    return [value for value, _, _ in lex(text)]


def occurrences(haystack, needle):
    return [i for i in range(len(haystack) - len(needle) + 1)
            if haystack[i:i + len(needle)] == needle]


def one(haystack, text, message):
    found = occurrences(haystack, tokens(text))
    need(len(found) == 1, message + ' (exactly one executable occurrence)')
    return found[0]


def balanced(items, start, opening, closing):
    need(items[start] == opening, 'balanced-region opening')
    depth = 0
    for i in range(start, len(items)):
        if items[i] == opening:
            depth += 1
        elif items[i] == closing:
            depth -= 1
            if depth == 0:
                return i
    raise ContractViolation('unclosed executable region')


@dataclass(frozen=True)
class Definition:
    body: tuple
    start: int
    end: int


def definitions(text, name):
    raw = lex(text)
    items = [item[0] for item in raw]
    found = []
    for begin in occurrences(items, tokens(name + '(')):
        opening = begin + len(tokens(name))
        end_args = balanced(items, opening, '(', ')')
        next_token = end_args + 1
        while next_token < len(items) and items[next_token] in ('const', 'noexcept', 'override', 'final'):
            next_token += 1
        # Calls and declarations are not definitions. Constructor initializer
        # lists are outside the methods checked by this finite contract.
        if next_token >= len(items) or items[next_token] != '{':
            continue
        end_body = balanced(items, next_token, '{', '}')
        found.append(Definition(tuple(items[next_token + 1:end_body]),
                                raw[next_token][2], raw[end_body][1]))
    return found


def function(text, name):
    found = definitions(text, name)
    need(len(found) == 1, 'one definition required: ' + name)
    return list(found[0].body)


WORK_METHODS = (
    'initialize', 'createBuffer', 'uploadBuffer', 'createTexture', 'uploadTexture',
    'createShader', 'createDeclaration', 'clear', 'draw', 'readback', 'readPresented',
    'issueEvent', 'eventComplete', 'destroy', 'finish',
)
OTHER_METHODS = {'active', 'quarantine', 'present', 'diagnosticMemory', 'diagnosticShaderCache'}


def check_admission(sources):
    header = tokens(sources['header'])
    one(header, 'enum class Admission { Work, WindowFaultCleanup };', 'finite admission authority')
    declaration = one(header, 'Owner& active(Admission admission = Admission::Work);',
                      'work admission must be the default')
    need('private' in header[:declaration], 'admission is backend-private')
    backend = tokens(sources['backend'])
    one(backend, 'Admission::WindowFaultCleanup', 'Present is the sole cleanup exception')
    present = function(sources['backend'], 'D3D9Backend::present')
    one(present, 'Owner& owner = active(Admission::WindowFaultCleanup);', 'Present cleanup authority')
    active = function(sources['backend'], 'D3D9Backend::active')
    one(active, 'require(m_owner != nullptr,', 'live owner precondition')
    one(active, 'require(m_owner->thread == GetCurrentThreadId(),', 'owner-thread precondition')
    suffix = tokens('if (admission == Admission::Work && m_owner->surfaceTarget) '
                    'm_owner->surfaceTarget->validate(); return *m_owner;')
    need(active[-len(suffix):] == suffix and active.count('return') == 1,
         'work validation must execute before the only owner return')
    need(active.count('validate') == 1, 'one work validation')
    for method in WORK_METHODS:
        body = function(sources['backend'], 'D3D9Backend::' + method)
        at = one(body, 'Owner& owner = active();', 'work entry ' + method)
        need(not occurrences(body[:at], tokens('owner.'))
             and not occurrences(body[:at], tokens('m_owner->')),
             'work entry precedes owner use: ' + method)
        need(len(occurrences(body, tokens('active('))) == 1,
             'one admitted owner per work entry: ' + method)
    # New backend methods must be explicitly classified, not silently skipped.
    names = set(re.findall(r'D3D9Backend::([A-Za-z_]\w*)\s*\(', sources['backend']))
    actual = {name for name in names if definitions(sources['backend'], 'D3D9Backend::' + name)}
    need(actual == set(WORK_METHODS) | OTHER_METHODS, 'new backend method requires admission classification')


def check_present_cleanup(sources):
    body = function(sources['backend'], 'D3D9Backend::present')
    inner = one(body, 'try { owner.validateWindow(); } catch (...) {',
                'Present keeps its original preflight cleanup branch')
    catch_open = inner + len(tokens('try { owner.validateWindow(); } catch (...)'))
    catch_end = balanced(body, catch_open, '{', '}')
    cleanup = body[catch_open + 1:catch_end]
    guard = one(cleanup, 'if (!owner.canCleanWindowFault(m_stats)) throw;',
                'unknown GPU state must not authorize fault cleanup')
    close = one(cleanup, 'owner.shutdown(m_stats, cleanupDeadline, true);',
                'allow-fault only in proven-quiescent cleanup')
    reset = one(cleanup, 'm_owner.reset();', 'fault cleanup owner reset')
    complete = one(cleanup, 'm_stats.faultCleanupCompleted = true;', 'fault cleanup completion')
    returned = one(cleanup, 'return D3DERR_DEVICELOST;', 'fault cannot become Present success')
    need(guard < close < reset < complete < returned and 'normalTeardown' not in cleanup,
         'fault cleanup ordering and classification')
    acquire = one(body, 'owner.presenter->acquireNextImage(', 'real Presenter acquisition')
    need(catch_end < acquire, 'window preflight and cleanup before acquisition')
    one(body[catch_end + 1:acquire], 'owner.healthy();', 'health before acquisition')
    predicate = function(sources['backend'], 'canCleanWindowFault')
    for witness in (
        'device->getDeviceStatus() != VK_SUCCESS', 'pending || commands != nullptr',
        'acquiredImage != nullptr || acquiredView != nullptr || resourcesInUse()',
        'stats.liveResources != 0 || stats.liveLogicalBytes != 0',
        'stats.submitted != stats.gpuCompleted || stats.gpuCompleted != stats.retired',
        'stats.presentQueued != stats.presentApiSucceeded',
        'stats.presentGpuCompleted != stats.presentRetired',
        'presentSignal->value() != lastPresentFrame',
    ):
        one(predicate, witness, 'quiescent cleanup witness: ' + witness)
    finish = function(sources['backend'], 'D3D9Backend::finish')
    finish_close = one(finish, 'owner.shutdown(m_stats, finishDeadline);', 'normal finish has no allow-fault')
    finish_reset = one(finish, 'm_owner.reset();', 'normal finish reset')
    normal = one(finish, 'm_stats.normalTeardown = true;', 'normal finish success')
    need(finish_close < finish_reset < normal, 'normalTeardown only after complete shutdown')


def check_shutdown(sources):
    body = function(sources['backend'], 'shutdown')
    # An exact executable chain catches missing, moved, conditionally skipped,
    # or comment-only health checks; three old health sites cannot mask it.
    chain = '''healthy(); device->waitForIdle(); healthy();
        stats.shutdownQueueDrained = true; unbindAuthorResources();
        if (presenter != nullptr) presenter->destroyResources();
        presenter = nullptr; healthy();
        if (surfaceTarget)
          surfaceTarget->closeAfterSurfaceRetired(operationDeadline, allowWindowFault);'''
    at = one(body, chain, 'post-Presenter health must precede HWND close')
    device_release = one(body, 'device = nullptr;', 'device retained through post-Presenter health')
    need(at + len(tokens(chain)) <= device_release, 'device must outlive HWND-close authorization')
    need(body.count('return') == 0, 'shutdown cannot bypass its retirement chain')
    one(tokens(sources['backend']), 'closeAfterSurfaceRetired(', 'one backend HWND-close exit')
    need(len(occurrences(tokens(sources['backend']), tokens('owner.shutdown('))) == 2,
         'only normal finish and proven fault-cleanup invoke shutdown')


def check_quarantine(sources):
    body = function(sources['backend'], 'D3D9Backend::quarantine')
    expected = tokens('''if (m_owner) {
        m_stats.quarantined = true;
        if (m_owner->surfaceTarget) {
          m_owner->surfaceTarget->onOwnerQuarantined();
          m_owner->surfaceTarget->report("owner-quarantine-snapshot");
        }
        (void)m_owner.release();
      }''')
    need(body == expected, 'quarantine must latch, report current snapshot, then retain owner')
    target = function(sources['target'], 'HostSurfaceTarget::onOwnerQuarantined')
    one(target, 'state->fail(Fault::OwnerQuarantined);', 'quarantine permanently latches fault')
    one(target, '(void)state->lifetime.abortBeforeSurface();', 'quarantine may abort only pre-surface')
    need(not occurrences(target, tokens('closeAfterSurfaceRetired('))
         and not occurrences(target, tokens('markSurfaceRetired(')),
         'quarantine never fabricates retirement')


def check_peer_identity(sources):
    backend = tokens(sources['backend'])
    init = function(sources['backend'], 'D3D9Backend::initialize')
    for field, value in (('window', 'window'), ('windowProcessId', 'windowProcessId'),
                         ('windowThreadId', 'windowThreadId')):
        one(backend, 'owner.' + field + ' =', 'peer field written once: ' + field)
        one(init, 'owner.' + field + ' = ' + value + ';', 'peer field never replaced: ' + field)
    one(init, 'owner.windowMarker.assign(windowMarker, markerLength);', 'original peer marker')
    one(init, 'owner.surfaceTarget->start(surfaceMode, window, windowProcessId, windowThreadId, '
              'authenticatedPeerProcess, windowMarker, initializationDeadline);',
        'authenticated peer identity passed unchanged into adapter')
    for field in ('windowProcessId', 'windowThreadId'):
        one(backend, 'm_stats.' + field + ' =', 'peer statistic written once: ' + field)
        one(init, 'm_stats.' + field + ' = ' + field + ';', 'peer statistic retains meaning: ' + field)
    host = tokens(sources['host'])
    one(host, 'peerPid(peer), peerProcess(process), surfaceMode(mode), marker(d9::WindowMarker(nonce))',
        'Session retains authenticated process and nonce')
    one(host, 'session = std::make_unique<Session>(DWORD(pid), nonce, parent.get(), surfaceMode);',
        'Session receives held OpenPeer handle')
    one(host, 'gpu->initialize(64, 64, window, peerPid, thread, marker.c_str(), '
              'presentedReadback, peerProcess, surfaceMode);', 'WSI backend gets real peer identity')


def check_surface_target(sources):
    selector = function(sources['backend'], 'surfaceWindow')
    need(selector == tokens('''if (surfaceTarget) return surfaceTarget->validatedWindow();
                              validateWindow(); return window;'''),
         'actual HWND selector must validate owned target and retain peer fallback')
    init = function(sources['backend'], 'D3D9Backend::initialize')
    claim = one(init, 'owner.surfaceTarget->claimSurface();', 'surface lifetime claimed once')
    presenter = one(init, 'owner.presenter = new Presenter(', 'original Presenter construction')
    callback = one(init, '[ownerPtr = &owner] (VkSurfaceKHR* surface) {', 'owner-bound surface factory')
    end = balanced(init, callback + len(tokens('[ownerPtr = &owner] (VkSurfaceKHR* surface)')), '{', '}')
    factory = init[callback:end + 1]
    selected = one(factory, 'const HWND actualWindow = ownerPtr->surfaceWindow();', 'select actual HWND')
    created = one(factory, 'const VkResult result = wsi::createSurface(actualWindow, '
                  'vki->getLoaderProc(), vki->instance(), surface);', 'production WSI uses actual HWND')
    observed = one(factory, 'ownerPtr->surfaceTarget->noteSurfaceResult(actualWindow, static_cast<int32_t>(result));',
                   'surface result reports same HWND and actual result')
    returned = one(factory, 'return result;', 'surface result not fabricated')
    need(claim < presenter < callback and selected < created < observed < returned,
         'surface claim and actual creation handoff order')
    need('claimSurface' not in factory, 'recreation must not reclaim lifetime')
    one(tokens(sources['backend']), 'wsi::createSurface(', 'one production surface creation path')


def check_offscreen(sources):
    host = function(sources['host'], 'apply')
    guard = one(host, 'if (init.profile == d9::PresentationProfile::Offscreen '
                     '&& surfaceMode != surface::Mode::Peer) return D3DERR_INVALIDCALL;',
                'owned offscreen mode must refuse explicitly')
    allocations = occurrences(host, tokens('gpu = std::make_unique<GPU>()'))
    need(allocations and guard < min(allocations), 'mode mismatch refused before GPU owner allocation')
    init = function(sources['backend'], 'D3D9Backend::initialize')
    identity = one(init, 'require(windowProcessId == 0 && windowThreadId == 0 && windowMarker == nullptr '
                        '&& authenticatedPeerProcess == nullptr && surfaceMode == surface::Mode::Peer,',
                   'backend offscreen requires empty identity and peer mode')
    instance = one(init, 'owner.instance = new DxvkInstance(', 'original DXVK instance')
    need(identity < instance, 'backend mode refusal precedes GPU initialization')
    one(tokens(sources['host']), 'surface::ReadStartupMode()', 'mode sampled once at host startup')
    one(tokens(sources['header']), 'HANDLE authenticatedPeerProcess = nullptr, '
                                  'surface::Mode surfaceMode = surface::Mode::Peer', 'peer remains API default')


CHECKS = (check_admission, check_present_cleanup, check_shutdown, check_quarantine,
          check_peer_identity, check_surface_target, check_offscreen)


def check_all(sources):
    for check in CHECKS:
        check(sources)


def replaced(sources, key, old, new, name=None):
    result = dict(sources)
    text = result[key]
    start, end = 0, len(text)
    if name:
        found = definitions(text, name)
        need(len(found) == 1, 'mutation function must resolve exactly once')
        start, end = found[0].start, found[0].end
    region = text[start:end]
    need(region.count(old) == 1, 'mutation anchor must occur once: ' + old)
    result[key] = text[:start] + region.replace(old, new, 1) + text[end:]
    need(result[key] != sources[key], 'mutation must change the actual source')
    return result


class HostSurfaceIntegrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.sources = {name: path.read_text(encoding='utf-8') for name, path in SOURCE_FILES.items()}
        # Mutants must never get credit because the unchanged baseline already
        # fails the reader (including a lexer/extraction mistake in this test).
        check_all(cls.sources)
        cls.mutations_killed = 0

    @classmethod
    def tearDownClass(cls):
        print('HOST_SURFACE_INTEGRATION mutationChecksKilled=' + str(cls.mutations_killed)
              + ' nativeTested=false gpuTested=false deviceLossInjected=false', flush=True)

    def reject(self, key, old, new, name=None):
        changed = replaced(self.sources, key, old, new, name)
        with self.assertRaises(ContractViolation):
            check_all(changed)
        type(self).mutations_killed += 1

    def test_all_formal_handoffs(self):
        check_all(self.sources)

    def test_lexer_handles_numeric_separators_and_nonexecutable_braces(self):
        text = '''constexpr auto ns = 5'000'000'000ull;
          // void D3D9Backend::fake() { active(); }
          const char* prose = "D3D9Backend::fake() { }";
          void D3D9Backend::real() { const char* s = "https://x/{ }"; active(); }
        '''
        self.assertIn("5'000'000'000ull", tokens(text))
        self.assertEqual(definitions(text, 'D3D9Backend::fake'), [])
        self.assertEqual(function(text, 'D3D9Backend::real').count('active'), 1)
        for prefix in ('R', 'u8R', 'uR', 'UR', 'LR'):
            with self.subTest(prefix=prefix), self.assertRaises(ContractViolation):
                tokens(prefix + '"(raw)"')

    def test_whitespace_and_comments_do_not_change_executable_contract(self):
        changed = dict(self.sources)
        changed['backend'] = changed['backend'].replace('presenter = nullptr;',
            'presenter /* not an ownership proof */ =\n nullptr ;')
        check_all(changed)

    def test_work_admission_default_cannot_be_cleanup(self):
        self.reject('header', 'active(Admission admission = Admission::Work)',
                    'active(Admission admission = Admission::WindowFaultCleanup)')

    def test_every_work_method_rejects_direct_owner_and_cleanup_bypass(self):
        for method in WORK_METHODS:
            for replacement in ('Owner& owner = *m_owner;',
                                'Owner& owner = active(Admission::WindowFaultCleanup);'):
                with self.subTest(method=method, replacement=replacement):
                    self.reject('backend', 'Owner& owner = active();', replacement,
                                'D3D9Backend::' + method)

    def test_work_validation_cannot_be_deleted_commented_or_conditionally_disabled(self):
        for replacement in (';', '// m_owner->surfaceTarget->validate();',
                            'if (false) m_owner->surfaceTarget->validate();',
                            'const char* note = "m_owner->surfaceTarget->validate();";'):
            with self.subTest(replacement=replacement):
                self.reject('backend', 'm_owner->surfaceTarget->validate();', replacement,
                            'D3D9Backend::active')

    def test_owner_return_cannot_precede_validation(self):
        self.reject('backend', '  if (admission == Admission::Work',
                    '  return *m_owner;\n  if (admission == Admission::Work', 'D3D9Backend::active')

    def test_unclassified_new_backend_method_is_not_ignored(self):
        changed = dict(self.sources)
        changed['backend'] += '\nvoid D3D9Backend::uncheckedDraw() { m_owner->begin(m_stats); }\n'
        with self.assertRaises(ContractViolation):
            check_all(changed)
        type(self).mutations_killed += 1

    def test_present_must_keep_the_cleanup_exception(self):
        self.reject('backend', 'active(Admission::WindowFaultCleanup)', 'active()', 'D3D9Backend::present')

    def test_present_fault_cleanup_requires_real_quiescent_predicate(self):
        for replacement in ('if (false)\n        throw;',
                            '// if (!owner.canCleanWindowFault(m_stats))\n        // throw;'):
            with self.subTest(replacement=replacement):
                self.reject('backend', 'if (!owner.canCleanWindowFault(m_stats))\n        throw;',
                            replacement, 'D3D9Backend::present')
        self.reject('backend', '|| acquiredImage != nullptr || acquiredView != nullptr || resourcesInUse()',
                    '|| acquiredImage != nullptr || acquiredView != nullptr', 'canCleanWindowFault')

    def test_fault_cleanup_cannot_be_success_or_normal_teardown(self):
        self.reject('backend', '      return D3DERR_DEVICELOST;',
                    '      return D3D_OK;', 'D3D9Backend::present')
        self.reject('backend', 'm_stats.faultCleanupCompleted = true;',
                    'm_stats.faultCleanupCompleted = true; m_stats.normalTeardown = true;',
                    'D3D9Backend::present')

    def test_finish_cannot_allow_fault_or_claim_success_before_shutdown(self):
        self.reject('backend', 'owner.shutdown(m_stats, finishDeadline);',
                    'owner.shutdown(m_stats, finishDeadline, true);', 'D3D9Backend::finish')
        self.reject('backend', 'owner.shutdown(m_stats, finishDeadline);',
                    'm_stats.normalTeardown = true; owner.shutdown(m_stats, finishDeadline);',
                    'D3D9Backend::finish')

    def test_post_presenter_health_cannot_be_missing_commented_or_pre_release(self):
        text = self.sources['backend']
        region = definitions(text, 'shutdown')[0]
        body = text[region.start:region.end]
        release = body.index('presenter = nullptr;')
        after = body.index('healthy();', release)
        old = body[release:after + len('healthy();')]
        for new in (old.rsplit('healthy();', 1)[0],
                    old.rsplit('healthy();', 1)[0] + '// healthy();',
                    'healthy(); ' + old.rsplit('healthy();', 1)[0],
                    old.rsplit('healthy();', 1)[0] + 'if (false) healthy();'):
            with self.subTest(replacement=new):
                self.reject('backend', old, new, 'shutdown')

    def test_hwnd_close_cannot_precede_presenter_release(self):
        self.reject('backend', 'presenter = nullptr;',
                    'surfaceTarget->closeAfterSurfaceRetired(operationDeadline, allowWindowFault); '
                    'presenter = nullptr;', 'shutdown')

    def test_quarantine_must_latch_and_report_before_owner_release(self):
        call = 'm_owner->surfaceTarget->report("owner-quarantine-snapshot");'
        for replacement in (';', '// ' + call,
                            'const char* note = "owner-quarantine-snapshot";',
                            '(void)m_owner.release(); ' + call):
            with self.subTest(replacement=replacement):
                self.reject('backend', call, replacement, 'D3D9Backend::quarantine')
        self.reject('backend', 'm_owner->surfaceTarget->onOwnerQuarantined();',
                    ';', 'D3D9Backend::quarantine')

    def test_quarantine_cannot_fabricate_surface_retirement(self):
        self.reject('target', '(void)state->lifetime.abortBeforeSurface();',
                    '(void)state->lifetime.markSurfaceRetired();', 'HostSurfaceTarget::onOwnerQuarantined')

    def test_peer_identity_and_statistics_cannot_be_replaced_with_host_identity(self):
        for old, new in (
            ('owner.window = window;', 'owner.window = owner.surfaceWindow();'),
            ('owner.windowProcessId = windowProcessId;', 'owner.windowProcessId = GetCurrentProcessId();'),
            ('owner.windowThreadId = windowThreadId;', 'owner.windowThreadId = GetCurrentThreadId();'),
            ('m_stats.windowProcessId = windowProcessId;', 'm_stats.windowProcessId = GetCurrentProcessId();'),
            ('m_stats.windowThreadId = windowThreadId;', 'm_stats.windowThreadId = GetCurrentThreadId();'),
            ('authenticatedPeerProcess, windowMarker, initializationDeadline',
             'GetCurrentProcess(), windowMarker, initializationDeadline'),
        ):
            with self.subTest(field=old):
                self.reject('backend', old, new, 'D3D9Backend::initialize')

    def test_host_must_pass_authenticated_process_and_original_peer_fields(self):
        self.reject('host', 'std::make_unique<Session>(DWORD(pid),nonce,parent.get(),surfaceMode)',
                    'std::make_unique<Session>(DWORD(pid),nonce,GetCurrentProcess(),surfaceMode)')
        self.reject('host', 'window,peerPid,thread,marker.c_str(),presentedReadback,peerProcess,surfaceMode',
                    'window,GetCurrentProcessId(),thread,marker.c_str(),presentedReadback,peerProcess,surfaceMode')

    def test_actual_surface_target_and_creation_result_cannot_be_forged(self):
        for old, new in (
            ('return surfaceTarget->validatedWindow();', 'return window;'),
            ('const HWND actualWindow = ownerPtr->surfaceWindow();', 'const HWND actualWindow = ownerPtr->window;'),
            ('wsi::createSurface(actualWindow,', 'wsi::createSurface(ownerPtr->window,'),
            ('noteSurfaceResult(actualWindow, static_cast<int32_t>(result))',
             'noteSurfaceResult(actualWindow, 0)'),
        ):
            with self.subTest(field=old):
                self.reject('backend', old, new)

    def test_lifetime_claim_is_once_outside_recreation_callback(self):
        self.reject('backend', 'owner.surfaceTarget->claimSurface();', ';', 'D3D9Backend::initialize')
        self.reject('backend', 'const HWND actualWindow = ownerPtr->surfaceWindow();',
                    'ownerPtr->surfaceTarget->claimSurface(); '
                    'const HWND actualWindow = ownerPtr->surfaceWindow();', 'D3D9Backend::initialize')

    def test_offscreen_owned_mode_cannot_fallback_or_reach_gpu(self):
        self.reject('host', 'init.profile==d9::PresentationProfile::Offscreen&&surfaceMode!=surface::Mode::Peer',
                    'init.profile==d9::PresentationProfile::Offscreen&&false')
        self.reject('host', 'return D3DERR_INVALIDCALL; // Explicit mode mismatch is not a fallback.',
                    'surfaceMode=surface::Mode::Peer; // implicit fallback')
        self.reject('backend', '&& authenticatedPeerProcess == nullptr && surfaceMode == surface::Mode::Peer',
                    '&& authenticatedPeerProcess == nullptr', 'D3D9Backend::initialize')


if __name__ == '__main__':
    unittest.main(verbosity=2)
