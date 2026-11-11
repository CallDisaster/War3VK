"""Source-only contract for the production/shared fixed-function compiler.

This checks the real source owner, explicit inputs, unchanged shader algorithms,
cache behavior and registration/dump ordering. It is NOT a C++ compilation,
SPIR-V equivalence, GPU draw, or game-coverage test. The integration owner must
run both-width compilation and real-key SPIR-V comparisons separately.

The preserved token hashes were taken before this boundary change from:
  d3d9_fixed_function.h SHA256
    09861B2970BA02BDC81B7D1B2763C93A4BC010F87609619CD04B4CE2DB6D466F
  d3d9_fixed_function.cpp SHA256
    BA2B79BB6FB9D7C9541131BF6AE682AE6EDAC8C537ACB8C6ABAD12BD09E26121
Only the four unused compiler Device parameters and the uber registration's
device accessor were normalized before hashing. No shader operation, binding,
constant, local layout, key hash/equality or ISGN token was normalized.

Run: python -B AutoTest/test_d3d9_fixed_function_shared_contract.py
All counterexamples are in memory; no build, DLL, process or game is changed.
"""

import hashlib
from pathlib import Path
import unittest

from test_d3d9_shared_boundary import (
    cpp_tokens, definitions, one_body, paired, require, sequence_positions,
    string_values,
)


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'src/d3d9/d3d9_fixed_function.cpp'
HEADER = ROOT / 'src/d3d9/d3d9_fixed_function.h'
ENTRY = 'CompileD3D9FixedFunctionShader'
PRESERVED = {
    'compiler': 'e9446a1ec29eed6e790696f5e1dd76a8cadde2073c5e0b0536e323b7e7391fdf',
    'local_layouts': '0c1263897d254897764b967be77c627ef44bb65cfa392926f55a43bd78d7f09f',
    'uber': 'dd497af1c1e0472fef025bbf9a1ee6460a823a9cedb6c4b1708f9048ee531ed1',
    'key_and_isgn': 'd60e12318564867b1a3b45e81d368671b7583d7e6f1d24ba18e3b57fd66c4b65',
}


def position(tokens, sequence):
    found = sequence_positions(tokens, sequence)
    require(len(found) == 1, f'exactly one token sequence required: {sequence}')
    return found[0]


def bodies(tokens, name, owner=None):
    return [tokens[start + 1:end] for start, end in definitions(tokens, name, owner)]


def preserved_sections(tokens):
    compiler = position(tokens, ['class', 'D3D9FFShaderCompiler', '{'])
    local = position(tokens, ['enum', 'class', 'D3D9FFVSMembers', '{'])
    entries = sequence_positions(tokens, ['Rc', '<', 'DxvkShader', '>', ENTRY, '('])
    require(len(entries) == 2 and compiler < entries[0], 'two shared compiler entries required')
    wrappers = bodies(tokens, 'D3D9FFShader', 'D3D9FFShader')
    require(len(wrappers) == 3, 'two specialized wrappers and one uber wrapper required')
    key_tail = position(tokens, cpp_tokens(
        'D3D9FFShaderKeyHash::operator () (const D3D9FFShaderKeyVS& key)'))
    return {
        'compiler': tokens[compiler:entries[0]],
        'local_layouts': tokens[local:compiler],
        'uber': wrappers[2],
        'key_and_isgn': tokens[key_tail:],
    }


def check_preserved(tokens):
    for name, span in preserved_sections(tokens).items():
        digest = hashlib.sha256('\n'.join(span).encode()).hexdigest()
        require(digest == PRESERVED[name], f'preserved shader tokens changed: {name}')


def check_no_game_device(tokens):
    for name in ('D3D9DeviceEx', 'GetDXVKDevice', 'GetOptions', 'pDevice'):
        require(name not in tokens, f'game device dependency: {name}')
    require(not any('d3d9_device.h' in value for value in string_values(tokens)),
            'full game device include is forbidden')


def check_entry_contract(tokens):
    entries = bodies(tokens, ENTRY)
    require(len(entries) == 2, 'exactly VS/FS compile entries required')
    for body, stage in zip(entries, ('VERTEX', 'FRAGMENT')):
        expected = cpp_tokens(f'''
            Sha1Hash hash = Sha1Hash::compute(&Key, sizeof(Key));
            DxvkShaderKey shaderKey = {{ VK_SHADER_STAGE_{stage}_BIT, hash }};
            std::string name = str::format("FF_", shaderKey.toString());
            D3D9FFShaderCompiler compiler(Key, name, options);
            return compiler.compile();
        ''')
        require(body == expected, 'CPU entry must retain naming and use only the shared compiler')


def check_wrapper_contract(tokens):
    wrappers = bodies(tokens, 'D3D9FFShader', 'D3D9FFShader')
    require(len(wrappers) == 3, 'exactly three wrapper constructors required')
    expected = cpp_tokens(f'''
        m_shader = {ENTRY}(Key, options);
        Dump(options);
        device->registerShader(m_shader);
    ''')
    require(wrappers[0] == expected and wrappers[1] == expected,
            'shipping specialized shaders must compile, dump, then register')
    require(tokens.count('registerShader') == 3, 'shader registration has three owners')
    guard = cpp_tokens('if (!War3GpuSkinVariant) device->registerShader(m_shader);')
    require(len(sequence_positions(wrappers[2], guard)) == 1,
            'private GPU-skin uber shader must not be prewarmed')


def check_cache_contract(tokens):
    methods = bodies(tokens, 'GetShaderModule', 'D3D9FFShaderModuleSet')
    require(len(methods) == 2, 'VS and FS must retain independent caches')
    for body, cache in zip(methods, ('m_vsModules', 'm_fsModules')):
        expected = cpp_tokens(f'''
            auto entry = {cache}.find(ShaderKey);
            if (entry != {cache}.end())
                return entry->second;
            D3D9FFShader shader(device, options, ShaderKey);
            {cache}.insert({{ShaderKey, shader}});
            return shader;
        ''')
        require(body == expected, 'cache hit must return before compile/register/insert')


def check_dump_contract(tokens):
    body = one_body(tokens, 'Dump', 'D3D9FFShader')
    expected = cpp_tokens('''
        const std::string& dumpPath = options->shaderDumpPath;
        if (dumpPath.size() != 0) {
            const std::string name = m_shader->debugName();
            std::ofstream dumpStream(
                str::topath(str::format(dumpPath, "/", name, ".spv").c_str()).c_str(),
                std::ios_base::binary | std::ios_base::trunc);
            m_shader->dump(dumpStream);
        }
    ''')
    require(body == expected, 'dump must preserve path/mode and stay fully opt-in')


class FixedFunctionSharedContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source_text = SOURCE.read_text('utf-8')
        cls.source = cpp_tokens(cls.source_text)
        cls.header = cpp_tokens(HEADER.read_text('utf-8'))

    def test_no_direct_game_device_dependency(self):
        check_no_game_device(self.source)
        check_no_game_device(self.header)

    def test_dependency_regressions_are_rejected(self):
        for regression in ('#include "d3d9_device.h"', 'class D3D9DeviceEx;',
                           'pDevice->GetDXVKDevice();', 'device->GetOptions();'):
            with self.subTest(regression=regression), self.assertRaises(ValueError):
                check_no_game_device(self.source + cpp_tokens(regression))

    def test_public_compile_entries_require_only_key_and_options(self):
        for key in ('D3D9FFShaderKeyVS', 'D3D9FFShaderKeyFS'):
            signature = cpp_tokens(f'Rc<DxvkShader> {ENTRY}(const {key}& Key, const D3D9Options* options)')
            self.assertEqual(len(sequence_positions(self.header, signature + [';'])), 1)
            self.assertEqual(len(sequence_positions(self.source, signature + ['{'])), 1)
        self.assertEqual(self.header.count(ENTRY), 2)

    def test_device_wrapper_signatures_are_explicit_and_non_retained(self):
        for key in ('D3D9FFShaderKeyVS', 'D3D9FFShaderKeyFS'):
            for method, last in (('D3D9FFShader', 'Key'), ('GetShaderModule', 'ShaderKey')):
                signature = cpp_tokens(f'{method}(const Rc<DxvkDevice>& device, const D3D9Options* options, const {key}& {last});')
                self.assertEqual(len(sequence_positions(self.header, signature)), 1)
        self.assertEqual(len(sequence_positions(self.header, cpp_tokens(
            'explicit D3D9FFShaderModuleSet(const Rc<DxvkDevice>& device);'))), 1)
        for name in ('D3D9FFShader', 'D3D9FFShaderModuleSet'):
            start = position(self.header, ['class', name]) + 2
            if self.header[start] == ':':
                start = self.header.index('{', start)
            end = paired(self.header, start)
            layout = self.header[start + 1:end]
            private = layout[layout.index('private') + 2:]
            self.assertNotIn('DxvkDevice', private)
            self.assertNotIn('D3D9Options', private)

    def test_cpu_entries_preserve_key_naming_and_use_the_production_compiler(self):
        check_entry_contract(self.source)

    def test_compile_entry_gpu_or_dump_side_effect_is_rejected(self):
        for addition in ('device->registerShader(shader);', 'Dump(options);',
                         'return nullptr;'):
            changed = self.source_text.replace('return compiler.compile();',
                                               addition + '\nreturn compiler.compile();', 1)
            with self.subTest(addition=addition), self.assertRaises(ValueError):
                check_entry_contract(cpp_tokens(changed))

    def test_compiler_has_no_device_parameter_or_member(self):
        compiler = preserved_sections(self.source)['compiler']
        self.assertNotIn('DxvkDevice', compiler)
        self.assertNotIn('registerShader', compiler)
        self.assertNotIn('D3D9Options', compiler)
        self.assertEqual(compiler.count('D3D9FixedFunctionOptions'), 5)

    def test_shipping_order_and_gpu_skin_no_prewarm_are_preserved(self):
        check_wrapper_contract(self.source)

    def test_removed_no_prewarm_guard_is_rejected(self):
        changed = self.source_text.replace('if (!War3GpuSkinVariant)', 'if (true)', 1)
        with self.assertRaises(ValueError):
            check_wrapper_contract(cpp_tokens(changed))

    def test_register_before_dump_is_rejected(self):
        changed = self.source_text.replace(
            'Dump(options);\n\n    device->registerShader(m_shader);',
            'device->registerShader(m_shader);\n\n    Dump(options);', 1)
        self.assertNotEqual(changed, self.source_text)
        with self.assertRaises(ValueError):
            check_wrapper_contract(cpp_tokens(changed))

    def test_both_cache_hit_and_miss_paths_are_preserved(self):
        check_cache_contract(self.source)

    def test_cache_hit_fallthrough_is_rejected(self):
        changed = self.source_text.replace('return entry->second;', ';', 1)
        with self.assertRaises(ValueError):
            check_cache_contract(cpp_tokens(changed))

    def test_uber_member_initialization_preserves_variants(self):
        expected = cpp_tokens('''
            D3D9FFShaderModuleSet::D3D9FFShaderModuleSet(const Rc<DxvkDevice>& device)
              : m_vsUbershader(device, DxsoProgramType::VertexShader)
              , m_fsUbershader(device, DxsoProgramType::PixelShader)
              , m_war3GpuSkinVsUbershader(device, DxsoProgramType::VertexShader, true) {}
        ''')
        self.assertEqual(len(sequence_positions(self.source, expected)), 1)

    def test_dump_uses_the_same_name_only_inside_enabled_branch(self):
        check_dump_contract(self.source)
        compiler = one_body(self.source, 'compile', 'D3D9FFShaderCompiler')
        self.assertEqual(len(sequence_positions(compiler, cpp_tokens('''
            m_module.setDebugSource(spv::SourceLanguageUnknown, 0,
                m_module.addDebugString(m_filename.c_str()), nullptr);
        '''))), 1)
        shader = cpp_tokens((ROOT / 'src/dxvk/dxvk_shader_spirv.cpp').read_text('utf-8'))
        metadata = one_body(shader, 'gatherMetadata', 'DxvkSpirvShader')
        self.assertEqual(len(sequence_positions(metadata, cpp_tokens('''
            case spv::OpSource: {
                if (ins.length() > 3u)
                    handleDebugName(code, ins.arg(3u));
            } break;
        '''))), 1)
        self.assertEqual(one_body(shader, 'debugName', 'DxvkSpirvShader'),
                         cpp_tokens('return m_debugName;'))

    def test_unconditional_dump_name_read_is_rejected(self):
        changed = self.source_text.replace('if (dumpPath.size() != 0)', 'if (true)', 1)
        with self.assertRaises(ValueError):
            check_dump_contract(cpp_tokens(changed))

    def test_options_still_copy_exactly_the_two_original_flags(self):
        self.assertEqual(one_body(self.source, 'D3D9FixedFunctionOptions', 'D3D9FixedFunctionOptions'),
                         cpp_tokens('''
                             invariantPosition = options->invariantPosition;
                             forceSampleRateShading = options->forceSampleRateShading;
                         '''))

    def test_shader_algorithms_local_layouts_uber_and_key_isgn_unchanged(self):
        check_preserved(self.source)

    def test_shader_algorithm_mutation_is_rejected(self):
        changed = self.source_text.replace('spvVersion(1, 3)', 'spvVersion(1, 4)', 1)
        with self.assertRaises(ValueError):
            check_preserved(cpp_tokens(changed))

    def test_single_source_owns_compiler_and_public_entries(self):
        compiler_owners = []
        entry_owners = []
        for domain in ('src', 'tools/render_host'):
            for path in sorted((ROOT / domain).rglob('*')):
                if not path.is_file() or path.suffix not in ('.h', '.hpp', '.cpp'):
                    continue
                source = path.read_text('utf-8')
                if 'D3D9FFShaderCompiler' not in source and ENTRY not in source:
                    continue
                tokens = cpp_tokens(source)
                compiler_owners.extend([path] * len(sequence_positions(
                    tokens, ['class', 'D3D9FFShaderCompiler', '{'])))
                entry_owners.extend([path] * len(definitions(tokens, ENTRY)))
        self.assertEqual(compiler_owners, [SOURCE])
        self.assertEqual(entry_owners, [SOURCE, SOURCE])


if __name__ == '__main__':
    unittest.main(verbosity=2)
