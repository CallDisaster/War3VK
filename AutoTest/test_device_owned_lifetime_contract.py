"""Source-shape guards for the device-owned lifetime fixture and owner wiring.

These are deliberately labelled static checks, not COM/GPU/game acceptance.
The companion fixture must still be compiled from real production objects and
run under the separately owned watchdog. No build or device is created here.
"""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]


def source(relative):
    return (ROOT / relative).read_text(encoding="utf-8")


def enum_values(text, name):
    body = re.search(rf"enum {name}\s*:\s*uint32_t\s*\{{(.*?)\}};", text, re.S)
    if not body:
        raise ValueError(f"missing enum {name}")
    pairs = re.findall(r"(\w+)\s*=\s*(\d+)", body[1])
    if len(pairs) != len(set(key for key, _ in pairs)):
        raise ValueError(f"duplicate enum field in {name}")
    return {key: int(value) for key, value in pairs}


def balanced(text, start, opening, closing):
    depth = 0
    for index in range(start, len(text)):
        if text[index] == opening:
            depth += 1
        elif text[index] == closing:
            depth -= 1
            if depth == 0:
                return text[start + 1:index], index + 1
    raise ValueError("unbalanced source block")


def no_comments(text):
    # Keep string literals; stripping comments prevents a claimed guard in a
    # comment from satisfying the source-only wiring assertions below.
    pattern = r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*.*?\*/'
    return re.sub(pattern, lambda m: "" if m[0].startswith(("//", "/*")) else m[0], text, flags=re.S)


class DeviceOwnedLifetimeContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.probe = source("AutoTest/test_device_owned_lifetime_probe.cpp")
        cls.parent = source("AutoTest/test_device_owned_lifetime_parent.cpp")

    def test_scalar_abi_matches_without_private_backend_struct(self):
        for name in ("Operation", "Field"):
            self.assertEqual(enum_values(self.probe, name), enum_values(self.parent, name))
        fields = enum_values(self.probe, "Field")
        self.assertEqual(len(fields), 39)
        self.assertEqual(set(fields.values()), set(range(39)))
        for text in (self.probe, self.parent):
            self.assertRegex(text, r"FieldCount\s*=\s*39;")
            self.assertNotIn("struct ImGui_ImplDX9_Data", text)
            self.assertNotIn("BackendRendererUserData", text)
        self.assertIn("operation > InstanceBufferArm", self.probe)

    def test_probe_is_not_added_to_shipping_target_or_exports(self):
        for path in ("src/d3d9/meson.build", "src/d3d9/d3d9.def", "meson.build"):
            text = no_comments(source(path))
            self.assertNotIn("WarVKTestDeviceOwned", text)
            self.assertNotIn("test_device_owned_lifetime_probe", text)
        self.assertIn('extern "C" __declspec(dllexport) HRESULT __cdecl WarVKTestDeviceOwned', self.probe)
        self.assertIn('GetProcAddress(state->module, "WarVKTestDeviceOwned")', self.parent)

    def test_real_draw_completion_readback_and_cursor_payload_evidence(self):
        for fragment in (
            "D3DQUERYTYPE_EVENT", "D3DISSUE_END", "D3DGETDATA_FLUSH",
            "DxvkStatCounter::CmdDrawCalls", "ui.newFrame()", "ui.render(false)",
            "GetRenderTargetData(target.ptr(), readback.ptr())",
            "difference >= -1 && difference <= 1", "!out[PixelFailures]",
            "command.GetTexID()", "texture->GetLevelDesc(0, &desc)",
            "if (actual != color) ++out[CursorPixelFailures]",
        ):
            self.assertIn(fragment, self.probe)
        self.assertLess(self.probe.index("ui.newFrame()"), self.probe.index("WantCaptureMouse = true"))
        self.assertIn("DrawUi(state, CursorSmallUpdate)", self.parent)
        self.assertNotIn("settings.bloom.enabled = false", self.probe)

    def test_partial_init_is_safe_format_rejection_not_oom_claim(self):
        self.assertIn("post.Init(Extent, Extent, D3DFMT_D16)", self.probe)
        self.assertNotIn("D3DFMT_VERTEXDATA", self.probe)
        self.assertIn("post.Init(Extent, Extent, D3DFMT_A8R8G8B8)", self.probe)
        normalize = no_comments(source("src/d3d9/d3d9_common_texture.cpp"))
        self.assertRegex(normalize, r"isDepthStencilFormat\s*&&\s*\(pDesc->Usage\s*&\s*D3DUSAGE_RENDERTARGET\)")
        self.assertIn(r'\"allocatorFailureInjected\":false', self.parent)

    def test_hidden_owned_windows_and_explicit_noncoverage(self):
        text = no_comments(self.parent)
        self.assertIn("WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW", text)
        self.assertIn("WS_POPUP, 0, 0, Width, Height", text)
        self.assertIn("GetMessageW", text)
        self.assertIn("PostMessageW(handle, WM_CLOSE, 0, 0)", text)
        for call in ("ShowWindow(", "SendInput(", "SetCursorPos(", "TerminateProcess(", "system("):
            self.assertNotIn(call, text)
        self.assertIn("++state.resetRejectNotEnabled", text)
        for field in ("gameLaunched", "globalInput", "visualGameAcceptance", "instanceDrawAcceptance"):
            self.assertIn(rf'\"{field}\":false', text)
        self.assertIn('"New device has unexpected internal public references"', text)
        self.assertIn('"Final device Release is not zero"', text)
        self.assertIn('"Final factory Release is not zero"', text)

    def test_instance_fixture_uses_native_storage_and_observes_tls_cleanup(self):
        for fragment in (
            "InstanceOwner::Get(device)", "InstanceOwner::GetActive(primaryInstanceDevice)",
            "!InstanceOwner::GetActive(device)", "buffer->Alloc(3, offset)",
            "buffer->Bind(1, offset)", "buffer->CaptureTextureState()",
            "buffer->RestoreTextureState()", "bound->GetDesc(&desc)",
            "bound->GetDevice(&owner)", "primaryInstanceDevice = nullptr;",
            "primaryInstances = nullptr;", "InstanceOwner::SetActive(buffer)",
        ):
            self.assertIn(fragment, self.probe)
        start = self.probe.index("void RunInstanceBuffer(")
        body, _ = balanced(self.probe, self.probe.index("{", start), "{", "}")
        self.assertNotIn("ReplayDraw", body)
        self.assertNotIn("DrawIndexedPrimitive", body)
        self.assertNotIn("primaryInstanceDevice->AddRef", body)
        self.assertIn("instance TLS active owner was not isolated/cleared", self.parent)
        self.assertIn("ArmInstanceBuffer(state, state.secondary.ptr);\n  Release(state, state.secondary);", self.parent)
        self.assertIn("ArmInstanceBuffer(state, state.primary.ptr);\n  Release(state, state.primary);", self.parent)
        self.assertIn("!buffer->Alloc(1, rejectedOffset)", body)
        self.assertIn("!buffer->Alloc(0, rejectedOffset)", body)
        self.assertIn("!buffer->Alloc(UINT32_MAX, rejectedOffset)", body)
        self.assertIn("batch < 2", body)
        self.assertIn("nextUnlock.unlock()", body)
        arm_start = self.probe.index("case InstanceBufferArm:")
        arm, _ = balanced(self.probe, self.probe.index("{", arm_start), "{", "}")
        self.assertIn("Instance* finalMapped = buffer->Alloc(1, finalOffset)", arm)
        self.assertIn("std::memcmp(finalMapped, &finalPayload, sizeof(finalPayload)) == 0", arm)
        arm = no_comments(arm)
        self.assertNotIn("Unlock()", arm)
        self.assertNotIn("InstanceUnlockScope", arm)

    def test_instance_production_ownership_is_per_device_and_private(self):
        header = no_comments(source("src/d3d9/war3/reimpl/war3_instance_buffer.h"))
        instance = no_comments(source("src/d3d9/war3/reimpl/war3_instance_buffer.cpp"))
        device_header = no_comments(source("src/d3d9/d3d9_device.h"))
        device = no_comments(source("src/d3d9/d3d9_device.cpp"))
        queue = no_comments(source("src/d3d9/war3/reimpl/war3_render_queue.h"))
        self.assertIn("std::unique_ptr<war3::reimpl::War3InstanceBuffer> m_war3InstanceBuffer", device_header)
        self.assertIn("device ? device->GetWar3InstanceBuffer() : nullptr", instance)
        self.assertNotIn("static War3InstanceBuffer s_buffer", instance)
        self.assertNotIn("GetInstBufPtr", queue)
        self.assertNotIn("s_activeLockedPtr", header + instance)
        self.assertIn("InstanceData *m_lockedPtr = nullptr", header)
        self.assertIn("Com<D3D9VertexBuffer, false> m_buffer", header)
        self.assertIn("Com<D3D9VertexDecl, false> original", header)
        self.assertIn("Com<D3D9VertexDecl, false> instanced", header)
        self.assertIn("s_activeBuffer->m_device == device", instance)
        self.assertIn("if (s_activeBuffer == this)", instance)
        self.assertLess(instance.index("TextureRefPrivate(texture.ptr(), true)"),
                        instance.index("TextureRefPrivate(m_capturedTextures[i], false)"))
        self.assertNotIn("War3InstanceBuffer::Get(this)->OnLostDevice()", device)
        self.assertNotIn("War3InstanceBuffer::Get(this)->OnResetDevice()", device)
        self.assertIn("m_war3InstanceBuffer->OnLostDevice()", device)
        self.assertIn("m_war3InstanceBuffer->OnResetDevice()", device)
        # Three mutating interception sites, plus the destructor's revoke gate.
        self.assertGreaterEqual(device.count("War3InstanceBuffer::GetActive(this)"), 4)
        self.assertIn("War3InstanceBuffer::GetActive(device)", device)

    def test_initial_public_owner_precedes_creating_private_children(self):
        interface = no_comments(source("src/d3d9/d3d9_interface.cpp"))
        begin = interface.index("D3D9InterfaceEx::CreateDeviceEx(")
        interface = interface[begin:]
        owner = interface.index("Com<D3D9DeviceEx> device = new D3D9DeviceEx(")
        reset = interface.index("device->InitialReset(")
        publish = interface.index("*ppReturnedDeviceInterface = device.ref();")
        self.assertLess(owner, reset)
        self.assertLess(reset, publish)

    def test_owner_revoked_before_gpu_drain_resources_released_after(self):
        device = no_comments(source("src/d3d9/d3d9_device.cpp"))
        start = device.index("D3D9DeviceEx::~D3D9DeviceEx()")
        body, _ = balanced(device, device.index("{", start), "{", "}")
        positions = [body.index(fragment) for fragment in (
            "beginDeviceTeardown(this)", "m_war3InstanceBuffer->Unlock()", "Flush()", "SynchronizeCsThread(",
            "m_dxvkDevice->waitForIdle()", "War3Imgui::get().shutdown(this)",
            "m_war3InstanceBuffer.reset()",
        )]
        self.assertEqual(positions, sorted(positions))
        ui = no_comments(source("src/d3d9/war3/ui/war3_imgui.cpp"))
        self.assertIn("static War3Imgui* instance = new War3Imgui", ui)
        self.assertNotIn("static War3Imgui instance;", ui)
        self.assertIn("m_cursorTexture = nullptr", ui)

    def test_both_wndproc_paths_gate_toggle_and_capture_by_window_owner(self):
        paths = (
            ("src/d3d9/d3d9_swapchain.cpp", "hWnd"),
            ("src/d3d9/d3d9_window.cpp", "window"),
        )
        for path, hwnd in paths:
            with self.subTest(path=path):
                text = no_comments(source(path))
                toggle_guarded = False
                for match in re.finditer(r"\bif\s*\(", text):
                    condition, end = balanced(text, text.index("(", match.start()), "(", ")")
                    if "VK_F1" in condition and "WM_KEYDOWN" in condition:
                        self.assertIn(f"ownsWindow({hwnd})", condition)
                        tail = text[end:].lstrip()
                        self.assertTrue(tail.startswith("{"))
                        body, _ = balanced(tail, 0, "{", "}")
                        self.assertIn("toggleVisible()", body)
                        toggle_guarded = True
                self.assertTrue(toggle_guarded, "UI toggle has no owner-gated condition")
                compact = re.sub(r"\s+", "", text)
                self.assertIn(f"constboolownsUi=War3Imgui::get().ownsWindow({hwnd});", compact)
                self.assertIn("constboolwantMouse=ownsUi&&War3Imgui::get().wantsCaptureMouse();", compact)
                self.assertIn("constboolwantKeyboard=ownsUi&&War3Imgui::get().wantsCaptureKeyboard();", compact)


if __name__ == "__main__":
    unittest.main()
