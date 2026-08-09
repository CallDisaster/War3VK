import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
PIPELINE_H = ROOT / "src/d3d9/d3d9_war3_pipeline.h"
PIPELINE_CPP = ROOT / "src/d3d9/d3d9_war3_pipeline.cpp"
WAR3_H = ROOT / "src/d3d9/war3/war3.h"
WAR3_CPP = ROOT / "src/d3d9/war3/war3.cpp"
DEVICE = ROOT / "src/d3d9/d3d9_device.cpp"
SOURCE_ROOT = ROOT / "src/d3d9"


class WarVkSettingsMailboxStaticTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.pipeline_h = PIPELINE_H.read_text(encoding="utf-8")
        cls.pipeline_cpp = PIPELINE_CPP.read_text(encoding="utf-8")
        cls.war3_h = WAR3_H.read_text(encoding="utf-8")
        cls.war3_cpp = WAR3_CPP.read_text(encoding="utf-8")
        cls.device = DEVICE.read_text(encoding="utf-8")

    def test_queued_pipeline_input_owns_an_immutable_snapshot(self):
        self.assertIn(
            "std::shared_ptr<const War3RenderSettings> settings",
            self.pipeline_h,
        )
        self.assertGreaterEqual(
            self.device.count("m_war3Pipeline->CaptureSettingsSnapshot(input)"),
            2,
        )
        self.assertNotIn("input.settings = &m_war3Pipeline->GetSettings()", self.device)

    def test_external_writers_use_a_shared_lifetime_mailbox(self):
        self.assertIn("struct War3RenderSettingsMailbox", self.pipeline_h)
        self.assertIn(
            "std::shared_ptr<War3RenderSettingsMailbox> mailbox",
            self.war3_cpp,
        )
        self.assertIn("std::atomic_load_explicit", self.war3_cpp)
        self.assertIn("s_activeSettingsMailbox", self.war3_cpp)
        self.assertNotIn("War3RenderSettings* GetMutableSettings()", self.war3_h)
        self.assertNotRegex(
            self.pipeline_h,
            r"War3RenderSettings\s*&\s*MutableSettings\s*\(",
        )

    def test_pending_settings_are_applied_only_at_frame_start(self):
        frame_start = self.pipeline_cpp[
            self.pipeline_cpp.index("void War3RenderPipeline::OnFrameStart()") :
            self.pipeline_cpp.index("bool War3RenderPipeline::NotifyDraw")
        ]
        self.assertIn("mailbox->mutex", frame_start)
        self.assertIn("mailbox->pendingRevision", frame_start)
        self.assertIn("m_settings = mailbox->pending", frame_start)
        self.assertIn("mailbox->pending = m_settings", frame_start)

    def test_cs_derived_lighting_does_not_mutate_the_authored_snapshot(self):
        shadow = (ROOT / "src/d3d9/d3d9_war3_shadow.cpp").read_text(
            encoding="utf-8"
        )
        self.assertNotIn("const_cast<War3RenderSettings", shadow)
        self.assertIn("input.lighting->sunDirection = finalLightDir", shadow)
        self.assertIn(
            "mailbox->pendingRevision == input.settingsRevision",
            shadow,
        )

    def test_device_revokes_publication_before_pipeline_destruction(self):
        destructor = self.device[
            self.device.index("D3D9DeviceEx::~D3D9DeviceEx()") :
            self.device.index("void D3D9DeviceEx::War3AttachGpuSkinNativeBridge")
        ]
        self.assertLess(
            destructor.index("war3::ClearActiveDeviceIfCurrent(this)"),
            destructor.index("DestroyWar3RenderPipeline"),
        )

    def test_active_and_mailbox_publication_order_is_fail_closed(self):
        publish = self.war3_cpp[
            self.war3_cpp.index("void PublishActiveDeviceLocked(") :
            self.war3_cpp.index("} // namespace")
        ]
        self.assertLess(
            publish.index("s_activeDevice.store(device"),
            publish.index("&s_activeSettingsMailbox"),
        )

        clear = self.war3_cpp[
            self.war3_cpp.index("bool ClearActiveDeviceIfCurrent(") :
            self.war3_cpp.index("bool IsActiveDevice(")
        ]
        self.assertLess(
            clear.index("&s_activeSettingsMailbox"),
            clear.index("s_activeDevice.store(nullptr"),
        )

    def test_no_call_site_keeps_the_old_mutable_settings_pointer(self):
        offenders = []
        pointer_pattern = re.compile(r"auto\s*\*[^;=]*=\s*(?:dxvk::war3::)?GetMutableSettings\s*\(")
        for path in SOURCE_ROOT.rglob("*"):
            if path.suffix not in {".cpp", ".h"}:
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            if pointer_pattern.search(text):
                offenders.append(str(path.relative_to(ROOT)))
        self.assertEqual([], offenders)


if __name__ == "__main__":
    unittest.main()
