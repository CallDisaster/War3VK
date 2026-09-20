"""Source/policy gates only; not native key, real GPU or player acceptance."""
import hashlib
import struct
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / 'src/d3d9/war3/tools'
CPP = (TOOLS / 'war3_async_screenshot.cpp').read_text(encoding='utf-8')
CORE = (TOOLS / 'war3_async_screenshot_core.h').read_text(encoding='utf-8')
SWAP = (ROOT / 'src/d3d9/d3d9_swapchain.cpp').read_text(encoding='utf-8')
HOOK = (ROOT / 'src/d3d9/war3/hooks/war3_native_capture.cpp').read_text(encoding='utf-8')


class AsyncScreenshotTests(unittest.TestCase):
    def test_gate_independent_of_profiler(self):
        self.assertIn('DXVK_WAR3_ASYNC_SCREENSHOT', CPP)
        for forbidden in ('isRecording', 'RecordingOwner', 'FRAME_TIMELINE', 'War3PerfMonitor'):
            self.assertNotIn(forbidden, CPP)

    def test_native_event_not_save_result(self):
        h = HOOK.split('static int __fastcall Hook_NativeScreenshotEvent', 1)[1].split('static void InstallNativeAsyncScreenshot', 1)[0]
        self.assertIn('code == 0x212u', h)
        self.assertIn('IsReadableRange(event, sizeof(code))', h)
        self.assertIn('RequestNativeAsyncScreenshot()) return 1', h)
        self.assertEqual(h.count('g_nativeScreenshotEventTrampoline(event)'), 1)
        for forbidden in ('WriteProcessMemory', 'SetAsyncKeyState', 'LockRect', 'TrySaveScreenshot'):
            self.assertNotIn(forbidden, h)

    def test_hook_exact_guard(self):
        h = HOOK.split('static void InstallNativeAsyncScreenshot',1)[1].split('void InstallNativeCapture',1)[0]
        for required in ('{2, 22}', 'array<uint8_t, 27>', '0x175530u',
                         'f2a84946771dc3853231d4a4116fb582cbe96853',
                         'NormalizeNativeFrameCode', 'identity && InstallMinHook'):
            self.assertIn(required, h)

    def test_read_only_pe_relocations(self):
        b = Path('E:/Work/Warcraft III/Game.dll').read_bytes()
        self.assertEqual(hashlib.sha256(b).hexdigest().upper(),
            'E04D1716603C075EB0C8E1E21CF1093A664ADC5249EFAB396BFA08D7B09D0C3A')
        u16 = lambda n: struct.unpack_from('<H', b, n)[0]
        u32 = lambda n: struct.unpack_from('<I', b, n)[0]
        pe = u32(60); sections = pe+24+u16(pe+20)
        def offset(rva):
            for i in range(u16(pe+6)):
                s = sections+i*40; start = u32(s+12)
                if start <= rva < start+u32(s+16): return u32(s+20)+rva-start
            raise AssertionError('RVA not file backed')
        raw = b[offset(0x175530):offset(0x175530)+27]
        self.assertEqual(raw.hex(), '8b15383abe6fb8010000008139120200000f44d08915383abe6fc3')
        self.assertEqual(hashlib.sha1(raw).hexdigest(), 'f2a84946771dc3853231d4a4116fb582cbe96853')
        d = pe+24+96+5*8; at = offset(u32(d)); end = at+u32(d+4); matches=[]
        while at < end:
            page, size = u32(at), u32(at+4)
            self.assertGreaterEqual(size,8); self.assertLessEqual(at+size,end)
            for n in range(8,size,2):
                v = u16(at+n); rva = page+(v&4095)
                if v>>12 and 0x175530 <= rva < 0x17554b: matches.append((rva-0x175530,v>>12))
            at += size
        self.assertEqual(matches, [(2,3),(22,3)])

    def test_no_renderer_readback_wait(self):
        h = SWAP.split('void D3D9SwapChainEx::CaptureNativeAsyncScreenshot(bool afterUi)',1)[1].split('HRESULT D3D9SwapChainEx::Reset(',1)[0]
        for forbidden in ('LockRect', 'GetRenderTargetData', 'WaitFor', 'Synchronize',
                          'FlushCs', 'waitIdle', 'save(', 'mapPtr', 'getValue'):
            self.assertNotIn(forbidden,h)
        self.assertLess(h.index('copyImageToBuffer('),h.index('signalFence('))
        self.assertIn('cImage = image',h)
        self.assertNotIn('[&',h)
        self.assertIn('quarantine(i)',h)

    def test_native_pixel_boundary_before_overlay_and_rotation(self):
        present = SWAP.split('HRESULT STDMETHODCALLTYPE D3D9SwapChainEx::Present(',1)[1]
        self.assertLess(present.index('CaptureNativeAsyncScreenshot();'),present.index('ProcessPendingFrameCapture('))
        self.assertLess(present.index('CaptureNativeAsyncScreenshot();'),present.index('PresentImage(presentInterval)'))

    def test_visibility_and_coherence(self):
        for token in ('VK_ACCESS_HOST_READ_BIT', 'VK_PIPELINE_STAGE_HOST_BIT',
                      'VK_MEMORY_PROPERTY_HOST_COHERENT_BIT', 'actualMemory & memory',
                      'storage()->getMemoryProperties()', 'memory | VK_MEMORY_PROPERTY_HOST_CACHED_BIT'):
            self.assertIn(token,CPP)
        context = (ROOT/'src/dxvk/dxvk_context.cpp').read_text(encoding='utf-8')
        release = context.split('void DxvkContext::releaseResources(',1)[1].split('void DxvkContext::syncResources(',1)[0]
        self.assertIn('e.buffer->info().access',release)
        barrier = (ROOT/'src/dxvk/dxvk_barrier.cpp').read_text(encoding='utf-8').split('void DxvkBarrierBatch::finalize(',1)[1]
        self.assertIn('VK_PIPELINE_STAGE_2_HOST_BIT',barrier)

    def test_gpu_completion_before_pixel_read(self):
        run = CPP.split('void run()',1)[1].split('AsyncScreenshot::AsyncScreenshot(',1)[0]
        self.assertLess(run.index('vkGetSemaphoreCounterValue('),run.index('saved = save(slot)'))
        self.assertLess(run.index('screenshot::Readable('),run.index('saved = save(slot)'))
        self.assertIn('query != VK_SUCCESS || !deviceOk',run)
        self.assertIn('State::Quarantined',run)
        self.assertIn('completed >= target',CORE)

    def test_memory_and_request_queue_bounded(self):
        self.assertIn('SlotCount = 3', CORE)
        self.assertIn('32ull * 1024 * 1024', CORE)
        request = CPP.split('bool AsyncScreenshot::request()',1)[1].split('void AsyncScreenshot::reset()',1)[0]
        self.assertIn('State::Free, State::Requested',request)
        self.assertIn('m->rejected.fetch_add(1)',request)
        self.assertIn('return true; // Handled but rejected',request)

    def test_reset_does_not_free_inflight_or_wait(self):
        reset = CPP.split('void AsyncScreenshot::reset()',1)[1].split('void AsyncScreenshot::prepare(',1)[0]
        self.assertIn('State::Requested, State::Retired',reset)
        self.assertNotIn('State::Submitted,',reset)
        for forbidden in ('join(', 'buffer =', 'fence =', 'wait('): self.assertNotIn(forbidden,reset)
        self.assertIn('State::Retired, State::Preparing',CPP)
        self.assertIn('slot.width != m->width',CPP)

    def test_worker_lifetime_and_registry(self):
        self.assertEqual(CPP.count('worker = std::thread('),1)
        self.assertIn('std::try_to_lock',CPP)
        self.assertIn('if (r.owner == this) r.owner = nullptr',CPP)
        self.assertIn('m_asyncScreenshot.release();',SWAP)
        self.assertIn('worker.join()',CPP)

    def test_no_fake_save_or_overwrite(self):
        save = CPP.split('bool save(const Slot& slot)',1)[1].split('void run()',1)[0]
        for token in ('GetModuleFileNameW(nullptr', 'Screenshots', 'WC3ScrnShot_',
                      'CREATE_NEW', 'MoveFileExW(partial.c_str(), path.c_str(), 0)', '.tga'):
            self.assertIn(token,save)
        self.assertLess(save.index('file.close()'),save.index('MoveFileExW('))
        self.assertLess(save.index('MoveFileExW('),save.index('[AsyncScreenshot] saved'))
        self.assertNotIn('MOVEFILE_REPLACE_EXISTING',save)

    def test_known_formats_and_native_fallback(self):
        for token in ('VK_SAMPLE_COUNT_1_BIT', 'VK_FORMAT_B8G8R8A8_UNORM',
                      'VK_FORMAT_B8G8R8A8_SRGB', 'info.numLayers == 1', 'info.mipLevels == 1'):
            self.assertIn(token,CPP)
        self.assertIn('if (!supported) { reset(); return; }',CPP)

    def test_encoder_is_shared_with_test_not_a_model(self):
        self.assertIn('screenshot::EncodeTga(',CPP)
        self.assertIn('pitch > SIZE_MAX / height',CORE)
        self.assertIn('h[17] = 0x28',CORE)
        self.assertIn('= 255;',CORE)

    def test_completion_timeout_never_reuses_unknown_gpu_work(self):
        timeout = CPP.split('readyAt - slot.submittedAt >',1)[1].split('continue;',1)[0]
        self.assertIn('std::chrono::seconds(10)',timeout)
        self.assertIn('State::Quarantined',timeout)
        self.assertNotIn('State::Free',timeout)
        self.assertNotIn('mapPtr',timeout)

    def test_internal_command_is_not_keyboard_injection_or_fake_success(self):
        h = HOOK.split('bool InvokeNativeScreenshotEventForTest()',1)[1].split('void InstallNativeCapture',1)[0]
        for token in ('IsInternalTestApiEnabled()', 'AsyncScreenshotEnabled()',
                      'GetCurrentThreadId() != GetMainLoopThreadId()', 'g_nativeScreenshotEventEntry(&event)'):
            self.assertIn(token,h)
        api = (TOOLS/'war3_internal_test_api.cpp').read_text(encoding='utf-8')
        command = api.split('command == "screenshot.native_async"',1)[1].split('command == "autotest.waypoint"',1)[0]
        for token in ('count < 1 || count > 4', 'count.is_number_integer()',
                      'response["saved"] = false', 'response["globalInputUsed"] = false'):
            self.assertIn(token,command)
        for forbidden in ('SendInput', 'PostMessage', 'keybd_event'):
            self.assertNotIn(forbidden,command+h)

if __name__ == '__main__': unittest.main()
