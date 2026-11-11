"""Source-only guardrails for the same-revision FF key compilation tool.

These token/body checks and in-memory mutations are NOT execution of the C++
parser or compiler. The owner must independently build both widths, run valid
and malformed fixtures, verify no-overwrite behavior, and compare every SPIR-V
byte/SHA. A successful tool receipt alone is not a game draw or memory saving.

Run: python -B AutoTest/test_fixed_function_key_tool_static.py
"""

from pathlib import Path
import unittest

from test_d3d9_shared_boundary import (
    cpp_tokens, one_body, paired, require, sequence_positions, string_values,
)


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'tools/render_host/dxvk_core/ff_key_compile_main.cpp'


def once(tokens, source):
    positions = sequence_positions(tokens, cpp_tokens(source))
    require(len(positions) == 1, f'one active sequence required: {source}')
    return positions[0]


def member_body(tokens, name):
    """The selected in-class methods allow override; do not match destructors."""
    found = []
    for index, token in enumerate(tokens[:-1]):
        if token != name or tokens[index + 1] != '(' or (index and tokens[index - 1] == '~'):
            continue
        start = paired(tokens, index + 1) + 1
        if start < len(tokens) and tokens[start] == 'override':
            start += 1
        if start < len(tokens) and tokens[start] == '{':
            found.append(tokens[start + 1:paired(tokens, start)])
    require(len(found) == 1, f'one non-destructor method definition required: {name}')
    return found[0]


def check_no_gpu_or_game_load(tokens):
    for name in ('DxvkInstance', 'createDevice', 'registerShader', 'submitCommandList',
                 'vkCreateInstance', 'vkCreateDevice', 'LoadLibraryA', 'LoadLibraryW',
                 'GetProcAddress', 'D3D9FFShaderModuleSet'):
        require(name not in tokens, f'CPU-only tool acquired a forbidden operation: {name}')
    for name in ('d3d9_device.h', 'dxvk_instance.h'):
        require(not any(name in value for value in string_values(tokens)),
                f'CPU-only tool acquired a forbidden include: {name}')


def check_decimal(tokens):
    body = one_body(tokens, 'decimal')
    digits = once(body, '''require(c >= '0' && c <= '9',
        "only unsigned decimal words are allowed");''')
    bound = once(body, '''require(result <= (std::numeric_limits<uint32_t>::max() - digit) / 10u,
        "fixture uint32 overflow");''')
    update = once(body, 'result = result * 10u + digit;')
    require(digits < bound < update, 'validate digits and overflow before updating uint32')


def check_parse_bounds(tokens):
    once(tokens, 'constexpr size_t kMaxFixtureBytes = 16u * 1024u;')
    once(tokens, 'constexpr size_t kMaxTokenBytes = 64u;')
    once(tokens, 'constexpr size_t kMaxKeysPerStage = 32u;')
    once(tokens, 'constexpr size_t kMaxShaderBytes = 8u * 1024u * 1024u;')
    count = one_body(tokens, 'count')
    once(count, 'require(value <= kMaxKeysPerStage, "fixture stage count exceeds limit");')
    token = one_body(tokens, 'token')
    once(token, 'require(m_offset - start < kMaxTokenBytes, "fixture token exceeds limit");')
    reader = one_body(tokens, 'readFixture')
    once(reader, 'std::array<char, kMaxFixtureBytes + 1u> bytes{};')
    once(reader, 'file.read(bytes.data(), std::streamsize(bytes.size()));')
    once(reader, 'require(length > 0 && size_t(length) <= kMaxFixtureBytes, "fixture is empty or exceeds byte limit");')
    once(reader, '''require(c >= 0x20u ? c <= 0x7eu : isFixtureSpace(char(c)),
        "fixture must be ASCII without BOM or control bytes");''')


def check_fixture_shape(tokens):
    body = one_body(tokens, 'readFixture')
    positions = [once(body, part) for part in (
        'input.expect("WARVK_FF_KEYS_V1");',
        'fixture.invariantPosition = input.boolean("invariantPosition");',
        'fixture.forceSampleRateShading = input.boolean("forceSampleRateShading");',
        'const uint32_t vertexCount = input.count("vsCount");',
        'const uint32_t fragmentCount = input.count("fsCount");',
        'require(vertexCount + fragmentCount != 0, "empty key sample is not accepted");',
        'fixture.vertex = readKeys<5>(input, "VS", vertexCount);',
        'fixture.fragment = readKeys<16>(input, "FS", fragmentCount);',
        'input.finish();',
    )]
    require(positions == sorted(positions), 'fixture order changed')
    once(one_body(tokens, 'boolean'), '''require(text == "0" || text == "1",
        "fixture boolean must be exactly 0 or 1");''')
    once(one_body(tokens, 'finish'), '''expect("END"); skipSpace();
        require(m_offset == m_source.size(), "trailing fixture input is forbidden");''')
    keys = one_body(tokens, 'readKeys')
    unique = once(keys, '''require(std::find(keys.begin(), keys.end(), words) == keys.end(),
        "duplicate fixture key within one stage");''')
    append = once(keys, 'keys.push_back(words);')
    require(unique < append, 'duplicate rejection must precede retention')


def check_output_ownership(tokens):
    main = one_body(tokens, 'wmain')
    parsed = once(main, 'const Fixture fixture = readFixture(std::filesystem::path(argv[2]));')
    new_dir = once(main, '''require(CreateDirectoryW(output.c_str(), nullptr) != FALSE,
        "output directory must be new and its parent must already exist");''')
    require(parsed < new_dir, 'malformed fixtures must not create output directories')
    created = member_body(tokens, 'OutputHandle')
    once(created, '''m_value = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);''')
    for name in ('CREATE_ALWAYS', 'OPEN_ALWAYS', 'TRUNCATE_EXISTING', 'DeleteFileW',
                 'DeleteFileA', 'RemoveDirectoryW', 'MoveFileW', 'remove_all', 'create_directories'):
        require(name not in tokens, f'no-overwrite ownership was weakened: {name}')
    write = one_body(tokens, 'write')
    flushed = once(write, 'require(FlushFileBuffers(m_value) != FALSE, "SPIR-V output flush failed");')
    closed = once(write, 'require(CloseHandle(completed) != FALSE, "SPIR-V output close failed");')
    require(flushed < closed, 'output must be flushed and closed before it is receipted')


def check_original_dump(tokens):
    body = one_body(tokens, 'compileKey')
    order = [once(body, part) for part in (
        'auto shader = dxvk::CompileD3D9FixedFunctionShader(key, &options);',
        'shader->dump(stream);',
        'require(stream.good(), "SPIR-V dump failed or exceeded byte limit");',
        'require(magic == kSpirvMagic, "SPIR-V dump has an invalid magic");',
        'OutputHandle output(directory / filename);',
        'output.write(bytes);',
        'return result;',
    )]
    require(order == sorted(order), 'dump, validate, exclusive write, receipt order changed')
    require('getCode' not in body, 'do not compare binding-patched code as original dump')
    once(member_body(tokens, 'xsputn'),
         'if (count < 0 || uint64_t(count) > kMaxShaderBytes - m_bytes.size()) return 0;')
    once(member_body(tokens, 'overflow'),
         'if (m_bytes.size() == kMaxShaderBytes) return traits_type::eof();')


class FixedFunctionKeyToolStaticTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.text = SOURCE.read_text('utf-8')
        cls.tokens = cpp_tokens(cls.text)

    def test_is_cpu_only_and_does_not_load_game_modules(self):
        check_no_gpu_or_game_load(self.tokens)
        once(one_body(self.tokens, 'requireNoGameModule'), '''
            require(GetModuleHandleW(L"Game.dll") == nullptr
                && GetModuleHandleW(L"War3.exe") == nullptr,
              "this CPU-only tool must not run inside a game process");
        ''')

    def test_gpu_or_game_dependency_mutations_are_rejected(self):
        for injection in ('device->registerShader(shader);', 'vkCreateInstance();',
                          'LoadLibraryW(L"Game.dll");', '#include "d3d9_device.h"'):
            with self.subTest(injection=injection), self.assertRaises(ValueError):
                check_no_gpu_or_game_load(self.tokens + cpp_tokens(injection))

    def test_exact_cli_and_native_key_dimensions(self):
        main = one_body(self.tokens, 'wmain')
        once(main, '''require(argc == 5 && std::wstring_view(argv[1]) == L"--input"
            && std::wstring_view(argv[3]) == L"--output",
            "usage: warvk-ff-key-compile --input <fixture> --output <new-directory>");''')
        for statement in ('static_assert(sizeof(dxvk::D3D9FFShaderKeyVS) == 20);',
                          'static_assert(sizeof(dxvk::D3D9FFShaderKeyFS) == 64);',
                          'static_assert(sizeof(dxvk::D3D9FFShaderStage) == 8);',
                          'static_assert(dxvk::caps::TextureStageCount == 8);'):
            once(self.tokens, statement)

    def test_bounded_ascii_input_and_exact_field_order(self):
        check_parse_bounds(self.tokens)
        check_fixture_shape(self.tokens)

    def test_integer_parser_checks_before_overflow(self):
        check_decimal(self.tokens)

    def test_weakened_integer_parser_is_rejected(self):
        changed = self.text.replace(
            'result <= (std::numeric_limits<uint32_t>::max() - digit) / 10u',
            'true', 1)
        with self.assertRaises(ValueError):
            check_decimal(cpp_tokens(changed))

    def test_removed_size_count_and_token_limits_are_rejected(self):
        for condition in ('value <= kMaxKeysPerStage', 'm_offset - start < kMaxTokenBytes',
                          'length > 0 && size_t(length) <= kMaxFixtureBytes'):
            changed = self.text.replace(condition, 'true', 1)
            with self.subTest(condition=condition), self.assertRaises(ValueError):
                check_parse_bounds(cpp_tokens(changed))

    def test_removed_trailing_empty_or_duplicate_rejection_is_rejected(self):
        for condition in ('m_offset == m_source.size()', 'vertexCount + fragmentCount != 0',
                          'std::find(keys.begin(), keys.end(), words) == keys.end()'):
            changed = self.text.replace(condition, 'true', 1)
            with self.subTest(condition=condition), self.assertRaises(ValueError):
                check_fixture_shape(cpp_tokens(changed))

    def test_outputs_are_new_exclusive_and_flushed(self):
        check_output_ownership(self.tokens)

    def test_create_always_mutation_is_rejected(self):
        changed = self.text.replace('CREATE_NEW', 'CREATE_ALWAYS', 1)
        with self.assertRaises(ValueError):
            check_output_ownership(cpp_tokens(changed))

    def test_original_spirv_dump_is_bounded_and_validated_before_writing(self):
        check_original_dump(self.tokens)

    def test_patched_get_code_cannot_replace_original_dump(self):
        changed = self.text.replace('shader->dump(stream);', 'shader->getCode(nullptr, nullptr);', 1)
        with self.assertRaises(ValueError):
            check_original_dump(cpp_tokens(changed))

    def test_real_options_are_explicit_and_no_device_is_constructed(self):
        main = one_body(self.tokens, 'wmain')
        once(main, 'dxvk::D3D9Options options(dxvk::Rc<dxvk::DxvkDevice>{}, config);')
        once(main, 'options.invariantPosition = fixture.invariantPosition;')
        once(main, 'options.forceSampleRateShading = fixture.forceSampleRateShading;')
        once(main, 'std::copy(words.begin(), words.end(), key.Data.Primitive);')
        once(main, 'key.Stages[stage].Primitive[word] = words[stage * 2u + word];')
        once(main, '''require(SetEnvironmentVariableW(L"DXVK_LOG_PATH", L"none") != FALSE,
            "cannot disable incidental log-file output");''')

    def test_success_is_last_and_receipt_does_not_claim_rendering_or_equivalence(self):
        main = one_body(self.tokens, 'wmain')
        once(main, 'requireNoGameModule(); emitReceipt(fixture, files); return 0;')
        strings = string_values(one_body(self.tokens, 'emitReceipt'))
        text = ''.join(strings)
        for evidence in ('"schema":"warvk-ff-key-compile/v1"', '"success":true',
                         '"processBits":', '"vsKeyBytes":', '"fsKeyBytes":',
                         '"invariantPosition":', '"forceSampleRateShading":',
                         '"vsCount":', '"fsCount":', '"noGpu":true',
                         '"gameRender":false', '"binaryEquivalenceProven":false',
                         '"files":[', '"debugName":'):
            self.assertIn(evidence, text)


if __name__ == '__main__':
    unittest.main(verbosity=2)
