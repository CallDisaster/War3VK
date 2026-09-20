"""Release source must include configuration inputs, not only C++ files."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ReleaseSourceInputs(unittest.TestCase):
    def test_recorder_template_exists_and_has_one_substitution(self):
        path = ROOT / 'src/d3d9/war3/tools/war3_frame_recorder_build.h.in'
        text = path.read_text(encoding='utf-8')
        self.assertEqual(text.count('@WARVK_INTERNAL_FRAME_RECORDER_DEFAULT@'), 1)
        self.assertIn('#ifndef WARVK_INTERNAL_FRAME_RECORDER_DEFAULT', text)

    def test_meson_and_template_agree(self):
        meson = (ROOT / 'src/d3d9/meson.build').read_text(encoding='utf-8')
        self.assertIn("input : 'war3/tools/war3_frame_recorder_build.h.in'", meson)
        self.assertIn("get_option('warvk_internal_frame_recorder') ? 1 : 0", meson)

    def test_release_version_and_final_identity(self):
        rc = (ROOT / 'src/d3d9/version.rc').read_text(encoding='utf-8')
        self.assertIn('FILEVERSION        1,22,1,0', rc)
        self.assertIn('FILEFLAGS          0', rc)
        self.assertIn('WarVK 1.22.01 Direct3D 9 Runtime', rc)
        self.assertNotIn('RC1', rc)

    def test_minhook_is_built_from_pinned_source_not_an_untracked_archive(self):
        meson = (ROOT / 'src/d3d9/meson.build').read_text(encoding='utf-8')
        self.assertNotIn('libMinHook.x86.a', meson)
        for name in ('buffer.c', 'hook.c', 'trampoline.c', 'hde/hde32.c'):
            self.assertIn('../minhook/src/' + name, meson)
        self.assertIn("static_library('warvk_minhook'", meson)
        self.assertIn('link_with           : [ minhook_lib ]', meson)


if __name__ == '__main__':
    unittest.main()
