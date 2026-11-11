"""CPU-only structural gates for the production/x64 shared D3D9 boundary.

Tokenize comments/literals and pair delimiters instead of comparing long source
strings. These are source/build-graph checks, not C++ semantic compilation or
GPU acceptance. In-memory counterexamples exercise the same predicates; no
repository file, build directory, DLL, process or game is modified.

Run: python -B AutoTest/test_d3d9_shared_boundary.py
"""

import ast
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
HELPER = ROOT / 'src/d3d9/d3d9_shader_helpers.cpp'
HELPERS = (
    'DoFixedFunctionFog', 'DoFixedFunctionAlphaTest', 'SetupRenderStateBlock',
    'SetupSamplerArray', 'LoadSampler', 'SetupSpecUBO', 'GetPointSizeInfoVS',
    'GetPointSizeInfoPS', 'GetPointCoord', 'GetSharedConstants',
)
C_TOKEN = re.compile(
    r'/\*.*?\*/|//[^\n]*|'
    r'(?:u8|u|U|L)?R"(?P<delimiter>[^ ()\\\t\r\n]{0,16})\(.*?\)(?P=delimiter)"|'
    r'(?:u8|u|U|L)?"(?:\\.|[^"\\])*"|(?:u8|u|U|L)?\'(?:\\.|[^\'\\])*\'|'
    r'[A-Za-z_]\w*|::|->|&&|\|\||==|!=|<=|>=|\S', re.DOTALL)
MESON_TOKEN = re.compile(r'\#[^\n]*|\'(?:\\.|[^\'\\])*\'|"(?:\\.|[^"\\])*"|[A-Za-z_]\w*|\S')


def require(value, message):
    if not value:
        raise ValueError(message)


def cpp_tokens(source):
    return [m.group() for m in C_TOKEN.finditer(source)
            if not m.group().startswith(('//', '/*'))]


def meson_tokens(source):
    return [m.group() for m in MESON_TOKEN.finditer(source) if not m.group().startswith('#')]


def paired(tokens, start):
    pairs = {'(': ')', '[': ']', '{': '}'}
    require(start < len(tokens) and tokens[start] in pairs, 'opening delimiter required')
    stack = []
    for index in range(start, len(tokens)):
        token = tokens[index]
        if token in pairs:
            stack.append(pairs[token])
        elif token in pairs.values():
            require(stack and token == stack[-1], 'mismatched delimiter')
            stack.pop()
            if not stack:
                return index
    raise ValueError('unterminated delimiter')


def definitions(tokens, name, owner=None):
    """Find named function definitions, not declarations/calls/comment decoys.

The selected non-template free helpers and context methods all have an
ordinary parameter list immediately followed by a body. A grammar expansion
(e.g. a new trailing noexcept) must update this small recognizer deliberately.
"""
    result = []
    for index, token in enumerate(tokens[:-1]):
        if token != name or tokens[index + 1] != '(':
            continue
        if owner is not None and tokens[max(0, index - 2):index] != [owner, '::']:
            continue
        close = paired(tokens, index + 1)
        if close + 1 < len(tokens) and tokens[close + 1] == '{':
            result.append((close + 1, paired(tokens, close + 1)))
    return result


def one_body(tokens, name, owner=None):
    ranges = definitions(tokens, name, owner)
    require(len(ranges) == 1, f'exactly one definition required: {name}')
    start, end = ranges[0]
    return tokens[start + 1:end]


def sequence_positions(tokens, sequence):
    size = len(sequence)
    return [index for index in range(len(tokens) - size + 1)
            if tokens[index:index + size] == sequence]


def assignment_call(tokens, variable, function):
    positions = sequence_positions(tokens, [variable, '=', function, '('])
    require(len(positions) == 1, f'unique build assignment required: {variable}')
    start = positions[0] + 3
    return tokens[start + 1:paired(tokens, start)]


def keyword_list(tokens, name):
    positions = sequence_positions(tokens, [name, ':', '['])
    require(len(positions) == 1, f'unique build list required: {name}')
    start = positions[0] + 2
    return tokens[start + 1:paired(tokens, start)]


def string_values(tokens):
    return [ast.literal_eval(token) for token in tokens if token.startswith(('"', "'"))]


def check_unique_helpers(units):
    locations = {name: [] for name in HELPERS}
    for path, tokens in units.items():
        for name in HELPERS:
            locations[name].extend([path] * len(definitions(tokens, name)))
    for name, owners in locations.items():
        require(owners == [HELPER], f'{name}: shared helper must have one source owner; got {owners}')


def descriptor_guard(tokens):
    """Return the if-statement span that owns the descriptor-buffer sync."""
    spans = []
    for index, token in enumerate(tokens[:-1]):
        if token != 'if' or tokens[index + 1] != '(':
            continue
        condition_end = paired(tokens, index + 1)
        condition = tokens[index + 2:condition_end]
        if not sequence_positions(condition, ['DxvkContextFeature', '::', 'DescriptorBuffer']):
            continue
        body_start = condition_end + 1
        if tokens[body_start] == '{':
            body_end = paired(tokens, body_start) + 1
        else:
            body_end = tokens.index(';', body_start) + 1
        if 'getSyncHandle' in tokens[body_start:body_end]:
            spans.append((index, body_end))
    require(len(spans) == 1, 'descriptor sync must have one DescriptorBuffer guard')
    return spans[0]


def check_descriptor_exit(context):
    end = one_body(context, 'endRecording', 'DxvkContext')
    flush = one_body(context, 'flushCommandList', 'DxvkContext')
    require(context.count('getSyncHandle') == 1 and context.count('setDescriptorSyncHandle') == 1,
            'descriptor-copy synchronization must have one context owner')
    require('getSyncHandle' not in flush and 'setDescriptorSyncHandle' not in flush,
            'flushCommandList must not exclusively or redundantly own descriptor sync')
    require(sequence_positions(end, ['m_cmd', '->', 'setDescriptorSyncHandle', '(',
                                    'm_descriptorWorker', '.', 'getSyncHandle', '(', ')', ')', ';']),
            'returned list must own the descriptor-worker synchronization point')
    start, stop = descriptor_guard(end)
    require(start <= end.index('getSyncHandle') < stop, 'sync escaped the feature guard')
    required = ('endCurrentCommands', 'relocateQueuedResources', 'cleanup', 'finalize', 'exchange')
    require(all(end.count(name) == 1 for name in required), 'finalization stages are missing/ambiguous')
    require(max(end.index(name) for name in required[:3]) < start < stop
            <= end.index('finalize') < end.index('exchange'),
            'descriptor sync must follow cleanup and precede finalization/return')


class SharedD3D9BoundaryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.context = cpp_tokens((ROOT / 'src/dxvk/dxvk_context.cpp').read_text('utf-8'))
        cls.units = {}
        name_pattern = re.compile(r'\b(?:' + '|'.join(HELPERS) + r')\b')
        for domain in ('src', 'tools/render_host'):
            for path in sorted((ROOT / domain).rglob('*')):
                if path.is_file() and path.suffix in ('.cpp', '.h', '.hpp'):
                    source = path.read_text('utf-8')
                    if name_pattern.search(source):
                        cls.units[path] = cpp_tokens(source)
        cls.meson = {}
        for domain in ('src', 'tools/render_host'):
            for path in sorted((ROOT / domain).rglob('meson.build')):
                cls.meson[path] = meson_tokens(path.read_text('utf-8'))

    def test_shared_helpers_have_one_definition_each(self):
        check_unique_helpers(self.units)

    def test_duplicate_helper_definition_is_rejected(self):
        changed = dict(self.units)
        duplicate = ROOT / 'src/d3d9/d3d9_fixed_function.cpp'
        changed[duplicate] = changed.get(duplicate, []) + cpp_tokens('uint32_t GetPointCoord(SpirvModule& s) { return 0; }')
        with self.assertRaises(ValueError):
            check_unique_helpers(changed)

    def test_comments_strings_calls_and_declarations_are_not_definitions(self):
        decoys = r'''// uint32_t GetPointCoord(SpirvModule& s) { return 0; }
          /* uint32_t GetSharedConstants(SpirvModule& s) { return 0; } */
          auto s = "uint32_t GetPointCoord() { return 0; }";
          auto r = R"audit(uint32_t GetPointCoord() { return 0; })audit";
          uint32_t GetPointCoord(SpirvModule&);
          void use() { value = GetPointCoord(module); if (GetPointCoord(module)) {} }
        '''
        self.assertEqual(definitions(cpp_tokens(decoys), 'GetPointCoord'), [])
        self.assertEqual(definitions(cpp_tokens(decoys), 'GetSharedConstants'), [])

    def test_dxso_options_do_not_require_the_d3d9_device_wrapper(self):
        for path in sorted((ROOT / 'src/dxso').rglob('*')):
            if path.is_file() and path.suffix in ('.cpp', '.h'):
                tokens = cpp_tokens(path.read_text('utf-8'))
                with self.subTest(path=path.name):
                    self.assertNotIn('D3D9DeviceEx', tokens)
                    self.assertFalse(any('d3d9_device.h' in literal for literal in string_values(tokens)))
        options = cpp_tokens((ROOT / 'src/dxso/dxso_options.h').read_text('utf-8'))
        self.assertTrue(sequence_positions(options, ['Rc', '<', 'DxvkDevice', '>', '&']))
        self.assertTrue(sequence_positions(options, ['const', 'D3D9Options', '&']))
        self.assertTrue(sequence_positions(options, ['uint32_t', 'vertexFloatConstantBytes']))
        helper = self.units[HELPER]
        self.assertNotIn('D3D9DeviceEx', helper)
        self.assertFalse(any('d3d9_device.h' in literal for literal in string_values(helper)))

    def test_shared_helper_is_compiled_only_into_dxso_library(self):
        mentions = []
        for path, tokens in self.meson.items():
            mentions.extend(path for literal in string_values(tokens)
                            if literal.replace('\\', '/').endswith('/d3d9_shader_helpers.cpp')
                            or literal == 'd3d9_shader_helpers.cpp')
        dxso_path = ROOT / 'src/dxso/meson.build'
        self.assertEqual(mentions, [dxso_path])
        tokens = self.meson[dxso_path]
        sources = assignment_call(tokens, 'dxso_src', 'files')
        self.assertEqual(string_values(sources).count('../d3d9/d3d9_shader_helpers.cpp'), 1)
        library = assignment_call(tokens, 'dxso_lib', 'static_library')
        self.assertEqual(library[:3], ["'dxso'", ',', 'dxso_src'])
        dependency = assignment_call(tokens, 'dxso_dep', 'declare_dependency')
        self.assertEqual(keyword_list(dependency, 'link_with'), ['dxso_lib'])

    def test_shipping_and_host_consume_the_same_dxso_dependency(self):
        shipping = assignment_call(self.meson[ROOT / 'src/d3d9/meson.build'], 'd3d9_dll', 'shared_library')
        host = assignment_call(self.meson[ROOT / 'tools/render_host/dxvk_core/meson.build'],
                               'render_host_d3d9', 'executable')
        for target in (shipping, host):
            self.assertEqual(keyword_list(target, 'dependencies').count('dxso_dep'), 1)
            self.assertEqual(keyword_list(target, 'dependencies').count('dxvk_dep'), 1)
        self.assertFalse(any(Path(name).name in ('d3d9_shader_helpers.cpp', 'd3d9_fixed_function.cpp')
                             for name in string_values(host)))
        source_build = self.meson[ROOT / 'src/meson.build']
        subdir = sequence_positions(source_build, ['subdir', '(', "'dxso'", ')'])
        self.assertEqual(len(subdir), 1)
        start = max(index for index in range(subdir[0]) if source_build[index] == 'if')
        condition = source_build[start:subdir[0]]
        self.assertIn("'enable_d3d9'", condition)
        self.assertIn("'warvk_render_host_core_dev'", condition)
        self.assertIn('or', condition)

    def test_descriptor_sync_belongs_to_complete_recording_exit(self):
        check_descriptor_exit(self.context)

    def test_old_flush_only_descriptor_sync_is_rejected(self):
        changed = list(self.context)
        start, _ = definitions(changed, 'endRecording', 'DxvkContext')[0]
        begin, end = descriptor_guard(one_body(changed, 'endRecording', 'DxvkContext'))
        begin += start + 1
        end += start + 1
        statement = changed[begin:end]
        del changed[begin:end]
        flush_start, _ = definitions(changed, 'flushCommandList', 'DxvkContext')[0]
        changed[flush_start + 1:flush_start + 1] = statement
        with self.assertRaises(ValueError):
            check_descriptor_exit(changed)

    def test_descriptor_sync_before_cleanup_is_rejected(self):
        changed = list(self.context)
        start, _ = definitions(changed, 'endRecording', 'DxvkContext')[0]
        begin, end = descriptor_guard(one_body(changed, 'endRecording', 'DxvkContext'))
        begin += start + 1
        end += start + 1
        statement = changed[begin:end]
        del changed[begin:end]
        changed[start + 1:start + 1] = statement
        with self.assertRaises(ValueError):
            check_descriptor_exit(changed)

    def test_duplicate_descriptor_sync_is_rejected(self):
        changed = list(self.context)
        start, _ = definitions(changed, 'endRecording', 'DxvkContext')[0]
        begin, end = descriptor_guard(one_body(changed, 'endRecording', 'DxvkContext'))
        statement = changed[start + 1 + begin:start + 1 + end]
        flush_start, _ = definitions(changed, 'flushCommandList', 'DxvkContext')[0]
        changed[flush_start + 1:flush_start + 1] = statement
        with self.assertRaises(ValueError):
            check_descriptor_exit(changed)

    def test_descriptor_handle_flushes_pending_partial_work(self):
        worker = cpp_tokens((ROOT / 'src/dxvk/dxvk_descriptor_worker.h').read_text('utf-8'))
        body = one_body(worker, 'getSyncHandle')
        self.assertEqual(body.count('flushBlock'), 1)
        self.assertLess(body.index('flushBlock'), body.index('return'))
        commands = cpp_tokens((ROOT / 'src/dxvk/dxvk_cmdlist.cpp').read_text('utf-8'))
        submit = one_body(commands, 'submit', 'DxvkCommandList')
        self.assertTrue(sequence_positions(submit, ['m_descriptorSync', '.', 'synchronize', '(', ')', ';']))


if __name__ == '__main__':
    unittest.main(verbosity=2)
