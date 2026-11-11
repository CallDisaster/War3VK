"""Pure loader-source counterexamples; no native launch, registry or GPU."""
from pathlib import Path
import unittest

from render_lab_layer_source import check_empty_source_trace, check_loader_module_trace, NO_SETTINGS


SOURCE = (Path(__file__).resolve().parent/'artifacts/model-empty-source').resolve()


def trace():
    return '\n'.join('[Vulkan Loader] '+line for line in (
        'INFO: '+NO_SETTINGS,
        'LAYER: Searching for implicit layer manifest files',
        'LAYER: In following locations:',
        'LAYER: '+str(SOURCE),
        'LAYER: Found no files'))


class SourceTests(unittest.TestCase):
    def test_actual_loader_identity(self):
        path = Path('C:/Windows/System32/vulkan-1.dll')
        text = 'INFO: Using Vulkan Loader '+str(path)+'\nINFO: Vulkan Loader Version 1.4.341\n'
        self.assertTrue(check_loader_module_trace(text, path)['actualLogPathBound'])
        for bad in ('', text.replace('1.4.341', '1.4.300'), text.replace('System32', 'Other'),
                    text+'INFO: Vulkan Loader Version 1.4.300\n', text.replace('INFO:','DEBUG:')):
            with self.subTest(bad=bad), self.assertRaises(ValueError): check_loader_module_trace(bad, path)

    def test_exact_repeated_blocks(self):
        self.assertEqual(check_empty_source_trace(trace()+'\n'+trace(), SOURCE)['discoveryBlocks'], 2)

    def test_environment_or_path_alone_not_proof(self):
        for value in ('VK_IMPLICIT_LAYER_PATH='+str(SOURCE), str(SOURCE), '', 'INFO: '+NO_SETTINGS):
            with self.subTest(value=value), self.assertRaises(ValueError):
                check_empty_source_trace(value, SOURCE)

    def test_every_discovery_field_required(self):
        for line in range(5):
            rows = trace().splitlines(); del rows[line]
            with self.subTest(line=line), self.assertRaises(ValueError):
                check_empty_source_trace('\n'.join(rows), SOURCE)

    def test_other_or_extra_path(self):
        for bad in (trace().replace(str(SOURCE), str(SOURCE)+'-other'),
                    trace().replace('LAYER: Found no files', 'LAYER: extra-path\nLAYER: Found no files')):
            with self.assertRaises(ValueError): check_empty_source_trace(bad, SOURCE)

    def test_second_block_cannot_be_unchecked(self):
        with self.assertRaises(ValueError):
            check_empty_source_trace(trace()+'\n'+trace().replace('Found no files','Found the following files:'), SOURCE)

    def test_errors_warnings_registry_and_settings_fail(self):
        bad = ('ERROR: loader_get_json failed', 'WARNING | LAYER: unknown warning',
               'ERROR', 'ERROR [X]: failed',
               'LAYER: Checking for Layer Manifest files in Registry at HKLM\\ImplicitLayers',
               'INFO: using vk_loader_settings.json', 'INFO: environment ignored because elevated')
        for line in bad:
            with self.subTest(line=line), self.assertRaises(ValueError):
                check_empty_source_trace(trace()+'\n[Vulkan Loader] '+line, SOURCE)

    def test_unexpected_layer_not_silently_allowed(self):
        for line in ('LAYER: Loading layer library overlay.dll', 'LAYER: Insert instance layer "overlay" (x.dll)',
                     'LAYER: Inserted device layer "overlay" (x.dll)'):
            with self.subTest(line=line), self.assertRaises(ValueError):
                check_empty_source_trace(trace()+'\n'+line, SOURCE)

    def test_validation_name_is_explicit_not_wildcard(self):
        value = trace()+'\nLAYER: Insert instance layer "VK_LAYER_KHRONOS_validation" (x.dll)'
        self.assertTrue(check_empty_source_trace(value, SOURCE, ('VK_LAYER_KHRONOS_validation',))['actualDiscoveryProven'])
        with self.assertRaises(ValueError):
            check_empty_source_trace(value.replace('KHRONOS_validation','foreign'), SOURCE, ('VK_LAYER_KHRONOS_validation',))

    def test_fake_channels_rejected(self):
        with self.assertRaises(ValueError):
            check_empty_source_trace(trace().replace('LAYER:','INFO:'), SOURCE)


if __name__ == '__main__':
    unittest.main()
