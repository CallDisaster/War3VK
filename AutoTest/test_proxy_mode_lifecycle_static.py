"""Proxy mode's lifecycle seams (phase 5.1-e): what the front end, the data
layer, the host and the game probe have to agree on across files.

Text checks only. The behaviour is proved in the sandbox by
run_x64_game_proxy_probe.py (resize, full screen, host=kill, notice=dismiss)
and by run_x64_phase52_ab_gate.py; this keeps the agreements from drifting.
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]


def read(relative):
    return (ROOT / relative).read_text(encoding='utf-8')


def between(text, start, end):
    begin = text.index(start)
    return text[begin:text.index(end, begin + len(start))]


OBSERVER = read('src/d3d9/war3/core/war3_d3d9_observer.h')
FRONT = read('tools/render_host/d3d9_proxy.cpp')
CONNECTION = read('tools/render_host/d3d9_connection.h')
HOST = read('tools/render_host/dxvk_core/d3d9_host_main.cpp')
MODE = read('src/d3d9/war3/platform/war3_proxy_mode.cpp')
SERVICES = read('src/d3d9/war3/platform/war3_proxy_services.cpp')
HOST_LOST_H = read('src/d3d9/war3/platform/war3_proxy_host_lost.h')
HOST_LOST = read('src/d3d9/war3/platform/war3_proxy_host_lost.cpp')
NOTICE = read('src/d3d9/war3/platform/war3_player_notice.cpp')
STARTUP = read('src/d3d9/war3/platform/war3_startup_error.cpp')
WINDOW = read('src/d3d9/d3d9_window.cpp')
MESON = read('src/d3d9/meson.build')
PROBE = read('AutoTest/run_x64_game_proxy_probe.py')


class TerminalFacts(unittest.TestCase):
    def test_the_callback_carries_one_facts_struct(self):
        facts = between(OBSERVER, 'struct WarVKD3D9TerminalFacts {', '};')
        for field in ('uint32_t size;', 'uint32_t result;', 'const char* kind;', 'const char* detail;',
                      'uint32_t hostExitKnown;', 'uint32_t hostExit;', 'uint64_t published;', 'uint64_t applied;',
                      'const wchar_t* hostLog;'):
            self.assertIn(field, facts)
        self.assertIn('const WarVKD3D9TerminalFacts* facts);', OBSERVER)

    def test_the_front_end_fills_every_fact_from_the_connection(self):
        call = between(FRONT, 'if(observer.terminal&&!terminalDelivered&&conn->terminalReport(report)) {', '    }\n')
        self.assertIn('conn->sessionMarks()', call)
        self.assertIn('marks.hostExit!=STILL_ACTIVE?1u:0u,marks.hostExit', call)
        self.assertIn('marks.published,marks.applied,marks.hostLog', call)
        self.assertIn('observer.terminal(observer.user,this,&facts);', call)

    def test_the_exit_code_is_known_before_the_cause_is_published(self):
        watch = between(CONNECTION, 'void watchRing(HANDLE host) {', 'HRESULT contentSend')
        self.assertLess(watch.index('GetExitCodeProcess(host,&exit)'), watch.index('Terminal::HostExited'))
        self.assertIn('hostLogPath=hostLog;', CONNECTION)

    def test_the_data_layer_checks_the_struct_and_does_not_return(self):
        handler = between(MODE, 'void __stdcall OnTerminal(', '\n}\n')
        self.assertIn('facts->size != sizeof(WarVKD3D9TerminalFacts)', handler)
        self.assertIn('ObserveTerminal(device, *facts);', handler)
        self.assertIn('ReportRenderHostLostAndExit(facts, context);', SERVICES)
        self.assertIn('[[noreturn]] void ReportRenderHostLostAndExit', HOST_LOST_H)


class HostLost(unittest.TestCase):
    def test_the_report_is_written_before_the_dialog_and_the_process_is_terminated(self):
        body = between(HOST_LOST, 'void ReportRenderHostLostAndExit(', '\n}\n')
        written, shown = body.index('notice::WriteReportFile('), body.index('notice::ShowErrorDialog(')
        ended = body.index('TerminateProcess(GetCurrentProcess(), kRenderHostLostExitCode);')
        self.assertLess(written, shown)
        self.assertLess(shown, ended)
        # Whatever the report does, the process still ends.
        self.assertLess(body.index('} catch (...) {'), ended)
        self.assertNotIn('ExitProcess', body)

    def test_the_report_names_what_the_design_asks_for(self):
        for field in ('"kind="', '"hostExit="', '"published="', '" applied="', '"hostLog="', '"exitCode="'):
            self.assertIn(field, HOST_LOST)

    def test_the_probe_knows_the_same_exit_code_and_title(self):
        code = re.search(r'kRenderHostLostExitCode = (0x[0-9A-Fa-f]+)u;', HOST_LOST_H).group(1)
        self.assertEqual(int(code, 16), int(re.search(r'HOST_LOST_EXIT = (0x[0-9A-Fa-f]+)', PROBE).group(1), 16))
        title = re.search(r'const std::string title = "([^"]+)";', HOST_LOST).group(1)
        probe_title = re.search(r'NOTICE_TITLE = "([^"]+)"', PROBE).group(1).encode('ascii').decode('unicode_escape')
        self.assertEqual(title, probe_title)

    def test_dialogs_follow_one_policy(self):
        self.assertIn('notice::DecideDialog("DXVK_WAR3_HOST_LOST_DIALOG")', HOST_LOST)
        self.assertIn('notice::DecideDialog("DXVK_WAR3_STARTUP_DIALOG")', STARTUP)
        # Shown by default only on the desktop the player looks at.
        self.assertIn('_stricmp(decision.desktop.c_str(), "Default") == 0', NOTICE)
        self.assertNotIn('MessageBoxW', STARTUP)
        self.assertNotIn('MessageBoxW', HOST_LOST)

    def test_the_new_files_are_built(self):
        for name in ('war3_player_notice.cpp', 'war3_proxy_host_lost.cpp', 'war3_proxy_casters.cpp'):
            self.assertIn(f"'war3/platform/{name}'", MESON)


class WindowSize(unittest.TestCase):
    def test_both_modes_rewrite_the_games_resolution_after_a_resize(self):
        resized = between(SERVICES, 'void ObserveResized(', '\n}\n')
        for text in (resized, between(WINDOW, 'if (SUCCEEDED(hr)) {', '} else {')):
            self.assertIn('TryOverrideWindowedClientSize(', text)
            self.assertIn('TryNotifyWindowedUiSizeChanged(', text)
        self.assertIn('device->GetCreationParameters(&creation)', resized)

    def test_proxy_mode_takes_the_in_process_dpi_awareness(self):
        apply = between(MODE, 'static void ApplyDpiAwareness() {', '\n}\n')
        self.assertIn('config.getOption<bool>("d3d9.dpiAware", true)', apply)
        self.assertIn('SetProcessDPIAware();', apply)
        create = between(MODE, 'IDirect3D9* CreateFactory(UINT sdkVersion) noexcept {', '\n}\n')
        self.assertLess(create.index('ApplyDpiAwareness();'), create.index('front->create(sdkVersion)'))

    def test_the_host_takes_the_peers_dpi_awareness_before_anything_else(self):
        main = between(HOST, 'int wmain(int argc,wchar_t**argv){', 'lab::CreateServer(')
        self.assertIn('AdoptPeerDpiAwareness(parent.get());', main)
        adopt = between(HOST, 'static void AdoptPeerDpiAwareness(HANDLE peer){', '\n}\n')
        self.assertIn('"GetProcessDpiAwareness"', adopt)
        self.assertIn('if(SUCCEEDED(queried)&&theirs!=0)applied=set(theirs);', adopt)


class FullScreen(unittest.TestCase):
    def test_only_the_monitors_own_mode_is_accepted_and_nothing_changes_the_display(self):
        resolve = between(FRONT, 'static bool ResolveFullscreen(', '\n}\n')
        self.assertIn('p.BackBufferWidth!=UINT(m.right-m.left)||p.BackBufferHeight!=UINT(m.bottom-m.top)', resolve)
        self.assertIn('if(!presenter||', resolve)
        self.assertNotIn('ChangeDisplaySettings', FRONT)

    def test_the_window_covers_the_monitor_before_the_host_is_told(self):
        initialize = between(FRONT, 'HRESULT Device::initialize(){', '\n}\n')
        self.assertLess(initialize.index('enterFullscreen(requestedMonitor)'), initialize.index('conn->call(h)'))
        reset = between(FRONT, 'HRESULT Device::Reset(D3DPRESENT_PARAMETERS* p){', 'd9::Header h;h.op=d9::Op::Reset;')
        self.assertIn('applyWindowMode(mode)', reset)
        self.assertIn('leaveFullscreen();', between(FRONT, 'Device::~Device(){', 'unhookWindow();'))

    def test_the_window_hook_follows_the_original_in_full_screen(self):
        proc = between(FRONT, 'static LRESULT CALLBACK DeviceWindowProc(', '\n}\n')
        self.assertIn('if(fullscreen&&message==WM_NCCALCSIZE&&wp==TRUE)return 0;', proc)
        # A windowed WM_SIZE stays with the front end; a full-screen one goes to the application.
        self.assertIn('if(!hook.previous||transition||(message==WM_SIZE&&!fullscreen))', proc)
        self.assertIn("if (message == WM_NCCALCSIZE && wparam == TRUE && isFullscreen)", WINDOW)


if __name__ == '__main__':
    unittest.main()
