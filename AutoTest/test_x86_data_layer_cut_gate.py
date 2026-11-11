import unittest

import run_x86_data_layer_cut_gate as gate


def obj(layer, defined=(), undefined=()):
    return {'layer': layer, 'defined': set(defined), 'undefined': set(undefined)}


class ClassifyTest(unittest.TestCase):
    def test_known_objects(self):
        cases = {
            'd3d9_device.cpp': 'device', 'd3d9_war3_shadow.cpp': 'effects',
            'd3d9_war3_hook.cpp': 'data', 'jass_war3_jass.cpp': 'data',
            'war3_hooks_war3_hook_render.cpp': 'data', 'war3_state_war3_render_state.cpp': 'data',
            'war3_render_war3_renderer.cpp': 'mixed', 'war3_gpu_skin_war3_gpu_skin_compute.cpp': 'mixed',
            'war3_shader_api.cpp': 'mixed', 'war3_shaderpack_image.cpp': 'effects',
            'war3_tools_war3_perf_monitor.cpp': 'tools', 'war3_ui_war3_imgui.cpp': 'ui',
            'war3_war3.cpp': 'runtime', 'war3_war3_user_example.cpp': 'data',
            'meson-generated_.._war3_imgui_dx9_backend.cpp': 'imgui'}
        for name, layer in cases.items():
            self.assertEqual(gate.classify(None, name), layer, name)
        self.assertIsNone(gate.classify(None, 'version'))
        self.assertEqual(gate.classify('dxvk', 'dxvk_device.cpp'), 'dxvk')

    def test_unknown_object_fails(self):
        for name in ('war3_newdir_x.cpp', 'other.cpp', 'war3_.cpp'):
            with self.assertRaises(ValueError):
                gate.classify(None, name)


class AnalyzeTest(unittest.TestCase):
    def test_growth_pulls_noncore_and_stops_at_core(self):
        objects = {
            'd3d9:hook': obj('data', ['h'], ['m', 'fx']),
            'd3d9:mixed': obj('mixed', ['m'], ['dev']),
            'd3d9:device': obj('device', ['dev', 'unused']),
            'd3d9:effect': obj('effects', ['fx']),
        }
        data, forward, reverse = gate.analyze(objects)
        self.assertEqual(data, {'d3d9:hook', 'd3d9:mixed'})
        self.assertEqual(dict(forward), {('d3d9:hook', 'd3d9:effect'): {'fx'},
                                         ('d3d9:mixed', 'd3d9:device'): {'dev'}})
        self.assertEqual(dict(reverse), {})

    def test_portable_or_data_copy_satisfies(self):
        objects = {
            'd3d9:a': obj('data', [], ['log', 'inline']),
            'util:log': obj('util', ['log']),
            'd3d9:device': obj('device', ['log', 'inline']),
            'd3d9:b': obj('data', ['inline']),
        }
        _, forward, _ = gate.analyze(objects)
        self.assertEqual(dict(forward), {})

    def test_reverse_needs_data_only_definers(self):
        objects = {
            'd3d9:device': obj('device', [], ['query', 'comdat', 'external']),
            'd3d9:effect': obj('effects', [], ['query']),
            'd3d9:data': obj('data', ['query', 'comdat']),
            'd3d9:other': obj('device', ['comdat']),
        }
        _, _, reverse = gate.analyze(objects)
        self.assertEqual(dict(reverse), {('d3d9:device', 'd3d9:data'): {'query'},
                                         ('d3d9:effect', 'd3d9:data'): {'query'}})

    def test_growable_object_reached_only_through_core_is_not_pulled(self):
        objects = {
            'd3d9:data': obj('data', [], ['dev']),
            'd3d9:device': obj('device', ['dev'], ['tool']),
            'd3d9:tool': obj('tools', ['tool']),
        }
        data, forward, reverse = gate.analyze(objects)
        self.assertEqual(data, {'d3d9:data'})
        self.assertEqual(dict(forward), {('d3d9:data', 'd3d9:device'): {'dev'}})
        self.assertEqual(dict(reverse), {})


class CompareTest(unittest.TestCase):
    def receipt(self, forward, reverse=()):
        edge = lambda symbols: {'edges': [{'symbols': sorted(symbols)}]}
        return {'schema': gate.SCHEMA, 'forward': edge(forward), 'reverse': edge(reverse)}

    def test_added_and_removed(self):
        delta = gate.compare(self.receipt({'a', 'c'}, {'r'}), self.receipt({'a', 'b'}, {'r'}))
        self.assertEqual(delta['forward'], {'added': ['c'], 'removed': ['b'], 'before': 2, 'after': 2})
        self.assertEqual(delta['reverse'], {'added': [], 'removed': [], 'before': 1, 'after': 1})

    def test_schema_mismatch(self):
        with self.assertRaises(ValueError):
            gate.compare(self.receipt({'a'}), {'schema': 'other'})


if __name__ == '__main__':
    unittest.main()
