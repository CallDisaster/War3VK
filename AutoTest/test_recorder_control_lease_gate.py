import os
import unittest
from run_recorder_control_lease_gate import split_windows, syntax_arguments


class SyntaxCommand(unittest.TestCase):
    def test_output_and_dependency_arguments_removed_not_executed(self):
        command = ['compiler', '-Iinclude', '-O3', '-MD', '-MQ', 'object.obj',
                   '-MF', 'dep.d', '-o', 'object.obj', '-c', 'source.cpp']
        self.assertEqual(syntax_arguments(command), ['compiler', '-Iinclude', '-O3', 'source.cpp',
                         '-fsyntax-only', '-fdiagnostics-color=never'])

    def test_missing_output_value_rejected(self):
        for switch in ('-o', '-MF', '-MT', '-MQ'):
            with self.subTest(switch=switch), self.assertRaises(ValueError):
                syntax_arguments(['compiler', switch])

    def test_unknown_output_forms_and_response_files_rejected(self):
        for switch in ('-oobject.obj', '-MFdep.d', '-MTtarget', '-MQtarget', '-M', '-MM',
                       '@uninspected.rsp', '-Wp,-MD,dep.d', '-save-temps=obj'):
            with self.subTest(switch=switch), self.assertRaises(ValueError):
                syntax_arguments(['compiler', switch, 'source.cpp'])

    def test_non_output_defines_and_includes_preserved(self):
        flags = ['compiler', '-DMODE=1', '-Ipath with spaces', '-pthread', '-std=c++17', 'source.cpp']
        self.assertEqual(syntax_arguments(flags)[:-2], flags)

    @unittest.skipUnless(os.name == 'nt', 'actual Windows command parser')
    def test_windows_paths_and_spaces_not_posix_unescaped(self):
        self.assertEqual(split_windows(r'"E:\Compiler Root\g++.exe" "-I..\src\d3d9" "-IE:\Vulkan SDK\Include" -c "..\src\source.cpp"'),
                         [r'E:\Compiler Root\g++.exe', r'-I..\src\d3d9', r'-IE:\Vulkan SDK\Include', '-c', r'..\src\source.cpp'])


if __name__ == '__main__':
    unittest.main()
