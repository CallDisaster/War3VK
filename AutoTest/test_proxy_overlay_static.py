"""Static contract of the settings panel in proxy mode (the x64 overlay, T2).

Reads source only; no build, game or device. The runtime evidence is the game
probe (run_x64_game_proxy_probe.py with test=ui.panel_open / ui.panel_status /
ui.panel_close). See docs/research/2026-11-17-x64-overlay-proxy.md and
docs/research/2026-11-17-x64-overlay-proxy-impl.md.
"""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read(path):
    return (ROOT / path).read_text(encoding="utf-8")


def body(text, signature):
    start = text.index("{", text.index(signature))
    depth, end = 1, start + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start + 1:end - 1]


def no_comments(text):
    return re.sub(r"//[^\n]*", "", text)


FRONT = read("tools/render_host/d3d9_proxy.cpp")
DEF = read("tools/render_host/d3d9_proxy.def")
OBSERVER = read("src/d3d9/war3/core/war3_d3d9_observer.h")
MODE = read("src/d3d9/war3/platform/war3_proxy_mode.cpp")
SERVICES = read("src/d3d9/war3/platform/war3_proxy_services.cpp")
OVERLAY = read("src/d3d9/war3/platform/war3_proxy_overlay.cpp")
PANEL = read("src/d3d9/war3/ui/war3_imgui.cpp")
PANEL_DEVICE = read("src/d3d9/war3/ui/war3_imgui_inprocess.cpp")
TEST_API = read("src/d3d9/war3/tools/war3_internal_test_api.cpp")
MESON = read("src/d3d9/meson.build")


class FrontEnd(unittest.TestCase):
    def test_no_resize_inside_a_callback(self):
        follow = body(FRONT, "void followWindow()")
        self.assertIn("!observing", follow)
        # The resize after the frame (and its `resized` callback) stays.
        present = body(FRONT, "HRESULT STDMETHODCALLTYPE Present(")
        self.assertLess(present.index("presentFrame("), present.rindex("followWindow();"))

    def test_reset_callback_before_losable_check(self):
        reset = body(FRONT, "HRESULT Device::Reset(D3DPRESENT_PARAMETERS* p)")
        call = reset.index("observer.reset(observer.user,this)")
        self.assertLess(reset.index("if(!conn->isOwner())"), call)
        self.assertLess(call, reset.index("if(losable){notReset=true;"))
        self.assertIn("if(observer.reset&&!observing){const Observing scope{observing};", reset)

    def test_observer_takes_both_layouts(self):
        setter = body(FRONT, "HRESULT setObserver(const WarVKD3D9Observer* value)")
        self.assertIn("offsetof(WarVKD3D9Observer,reset)", setter)
        self.assertIn("std::memcpy(&observer,value,value->size)", setter)

    def test_private_reference_exports(self):
        own = body(FRONT, "static HRESULT WithOwnResource(")
        self.assertIn("d->conn->isOwner()", own)
        for kind in ("VertexBuffer", "IndexBuffer", "Texture"):
            self.assertIn(f"dynamic_cast<{kind}*>(resource))return r->parent==d?f(*r):D3DERR_INVALIDCALL;", own)
        retain = body(FRONT, "HRESULT RetainPrivate(Device* d,IUnknown* resource)")
        self.assertIn("if(r.publicReferencesZero())return D3DERR_INVALIDCALL;", retain)
        self.assertLess(retain.index("r.AddRefPrivate();"), retain.index("r.Release();"))
        self.assertIn("r.ReleasePrivate();", body(FRONT, "HRESULT ReleasePrivate(Device* d,IUnknown* resource)"))
        for line in ("WarVKD3D9RetainPrivate@8", "WarVKD3D9ReleasePrivate@8"):
            self.assertIn(line, DEF.split())
        self.assertIn("factory->GetAdapterDisplayMode(0,mode)", body(FRONT, "HRESULT Device::GetDisplayMode("))


class Observer(unittest.TestCase):
    def test_reset_is_the_last_member(self):
        struct = body(OBSERVER, "struct WarVKD3D9Observer")
        members = re.findall(r"\(__stdcall\*\s*(\w+)\)", struct)
        self.assertEqual(members, ["draw", "present", "resized", "terminal", "released", "reset"])
        for name in ("WarVKD3D9RetainPrivate", "WarVKD3D9ReleasePrivate"):
            self.assertIn(f"long __stdcall {name}(IDirect3DDevice9* device, IUnknown* resource);", OBSERVER)

    def test_data_layer_fills_reset_and_releases_panel_first(self):
        connect = body(MODE, "void ConnectCreatedDevice(")
        self.assertIn("observer.reset = &OnReset;", connect)
        self.assertLess(connect.index("War3Hook::InstallHooks(device);"), connect.index("OverlayConnect(device, overlay);"))
        released = body(MODE, "void __stdcall OnReleased(")
        self.assertLess(released.index("OverlayReleased(device);"), released.index("DisconnectDevice(device);"))
        self.assertIn("OverlayReset(device);", body(MODE, "void __stdcall OnReset("))


class DataLayer(unittest.TestCase):
    def test_panel_drawn_only_from_present_after_records_and_capture(self):
        present = body(SERVICES, "void ObservePresent(IDirect3DDevice9* device)")
        draw = present.index("OverlayPresent(device);")
        self.assertLess(present.index("FramePresent(device);"), draw)
        self.assertLess(present.index("tools::ProcessPendingFrameCapture("), draw)
        for path in ("src/d3d9/war3/platform", "src/d3d9/war3/hooks", "src/d3d9/war3/tools"):
            for source in (ROOT / path).glob("*.cpp"):
                text = no_comments(source.read_text(encoding="utf-8", errors="replace"))
                if source.name not in ("war3_proxy_services.cpp", "war3_proxy_overlay.cpp"):
                    self.assertNotIn("OverlayPresent(", text, source.name)

    def test_lazy_open_full_release_on_close(self):
        present = body(OVERLAY, "void OverlayPresent(IDirect3DDevice9* device)")
        self.assertLess(present.index("ClosePanel(device"), present.index("ui.initializeProxy(g_window, device)"))
        self.assertIn("device->TestCooperativeLevel() != D3D_OK", present)
        self.assertIn("ui.shutdownProxy(device);", body(OVERLAY, "void ClosePanel("))
        self.assertIn("ClosePanel(device", body(OVERLAY, "void OverlayReleased(IDirect3DDevice9* device)"))
        init = body(PANEL, "bool War3Imgui::initializeProxy(")
        self.assertIn("initializeContext(hwnd, device, kProxyRenderer, 1024);", init)
        context = body(PANEL, "void War3Imgui::initializeContext(")
        self.assertLess(context.index("io.Fonts->TexMaxWidth = io.Fonts->TexMaxHeight = textureLimit;"),
                        context.index("AddFontFromFileTTF"))

    def test_window_subclass_as_in_process(self):
        proc = no_comments(body(OVERLAY, "LRESULT CALLBACK PanelWindowProc("))
        self.assertIn("message == kPanelTestMessage && g_routing", proc)
        self.assertIn("message == WM_KEYDOWN && wp == VK_F1 && (::GetKeyState(VK_CONTROL) & 0x8000)", proc)
        self.assertLess(proc.index("IsImeMessage(message)"), proc.index("ui.handleWndProc("))
        self.assertIn("(IsMouseMessage(message) && wantMouse) || (IsKeyMessage(message) && wantKeyboard) || handled", proc)
        self.assertIn("return Forward(hook, window, message, wp, lp);", proc)

    def test_shared_panel_links_no_device_or_effects_code(self):
        shared = no_comments(PANEL)
        for forbidden in ("War3ImGui_ImplDX9_", "m_device->BeginScene", "m_device->EndScene",
                          "m_device->CreateTexture", "pipeline->", "dxvk::QueryShadowTaaDiagnostics",
                          "dxvk::QueryCsmResolutionDiagnostics", "LockRect", "GetLevelDesc"):
            self.assertNotIn(forbidden, shared)
        self.assertEqual(re.findall(r"war3shader::(?!internal::)\w+", shared), [])
        for query in ("QueryShadowTaaDiagnostics", "QueryCsmResolutionDiagnostics", "QueryShadowReplayDiagnostics",
                      "QueryWar3WeatherDiagnostics", "QueryWar3WeatherVolumeDiagnostics"):
            for call in re.findall(r"[\w:().]*" + query + r"\(", shared):
                self.assertTrue(call.startswith("ProcessServices()."), call)
        device = no_comments(PANEL_DEVICE)
        for defined in ("void War3Imgui::initialize(HWND hwnd, D3D9DeviceEx *device)",
                        "void War3Imgui::setCursorBitmap("):
            self.assertIn(defined, device)
            self.assertNotIn(defined, shared)
        self.assertIn("m_devicePanels.passToggles = &DrawPassToggles;", device)
        self.assertIn("m_devicePanels.shaderPack = &DrawShaderPackPanel;", device)

    def test_proxy_frame_skips_an_atlas_not_uploaded(self):
        render = body(PANEL, "void War3Imgui::render(bool inScene)")
        self.assertIn("if (m_target && (!m_proxy || uploadTextures(drawData)))", render)
        upload = body(PANEL, "bool War3Imgui::uploadTextures(ImDrawData* drawData)")
        self.assertIn("m_renderer->updateTexture(texture);", upload)
        self.assertIn("texture->Status != ImTextureStatus_Destroyed", upload)


class TestsAndBuild(unittest.TestCase):
    def test_commands_and_probe(self):
        for command in ("ui.panel_open", "ui.panel_close", "ui.panel_status"):
            self.assertIn(f'"{command}"', TEST_API)
            self.assertIn(f'"{command}"', read("AutoTest/run_x64_game_proxy_probe.py"))
        self.assertIn("proxy::RoutePanelRequest(open)", TEST_API)

    def test_meson(self):
        self.assertIn("'war3/platform/war3_proxy_overlay.cpp',", MESON)
        self.assertIn("'war3/ui/war3_imgui_inprocess.cpp',", MESON)
        target = MESON[MESON.index("warvk_imgui_dx9_proxy_backend = custom_target("):]
        target = target[:target.index(")\n") if ")\n" in target else target.index(")\r\n")]
        self.assertIn("'--variant', 'proxy'", target)
        self.assertIn("output : 'war3_imgui_dx9_proxy_backend.cpp'", target)
        self.assertRegex(MESON, r"d3d9_dll = shared_library\([^)]*warvk_imgui_dx9_proxy_backend")


if __name__ == "__main__":
    unittest.main()
