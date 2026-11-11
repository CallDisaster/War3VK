"""Source contract for the shared owner; actual compiler races are C++ tests.

This test is intentionally not GPU/memory-slope evidence. The native test
links the real shared implementation; no Python model substitutes for it.
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]


def source(name):
    return (ROOT / name).read_text(encoding="utf-8")


class SharedOwnerContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.header = source("src/dxso/dxso_shader_module_cache.h")
        cls.core = source("src/dxso/dxso_shader_module_cache.cpp")
        cls.production = source("src/d3d9/d3d9_shader.cpp")
        cls.host = source("tools/render_host/dxvk_core/d3d9_backend.cpp")

    def test_both_consumers_use_shared_owner(self):
        for text in (self.production, self.host):
            self.assertIn("std::make_unique<DxsoShaderModuleCache>", text)
            self.assertIn("->getOrCompile(", text)
            self.assertNotIn("module.compile(", text)
        self.assertIn("'dxso_shader_module_cache.cpp'", source("src/dxso/meson.build"))

    def test_exact_content_key_not_digest_only(self):
        self.assertIn("digest.eq(other.digest) && tokens == other.tokens", self.header)
        self.assertIn("Sha1Hash::compute(key.tokens.data(), byteCount)", self.core)
        self.assertIn("std::memcpy(key.tokens.data(), bytecode, byteCount)", self.core)
        self.assertIn("analysis.bytecodeByteLength != byteCount", self.core)
        self.assertNotIn("shaderSlot", self.core)

    def test_domain_all_effective_options_and_layout(self):
        declared = re.findall(r"(?:bool|D3D9FloatEmulation)\s+(\w+)\s*=", source("src/dxso/dxso_options.h"))
        self.assertEqual(len(declared), 9)
        for field in declared:
            self.assertIn(f"a.{field} == b.{field}", self.core)
        for field in ("floatCount", "intCount", "boolCount", "bitmaskCount"):
            self.assertIn(f"a.{field} == b.{field}", self.core)
        self.assertIn("sameLayout(vertexLayout, other.vertexLayout)", self.core)
        self.assertIn("sameLayout(pixelLayout, other.pixelLayout)", self.core)
        self.assertIn("deviceIdentity == other.deviceIdentity", self.core)
        self.assertLess(self.core.index("!m_domain.matches(domain)"), self.core.index("++m_stats.requests"))

    def test_winner_registration_after_validation_and_preallocation(self):
        validate = self.core.index("validateModule(candidate, true, validate)")
        second_lookup = self.core.index("m_entries.find(key)", validate)
        allocate = self.core.index("m_entries.emplace", second_lookup)
        register = self.core.index("m_register(candidate->shader)")
        ready = self.core.index("entry.state = State::Ready", register)
        self.assertLess(validate, second_lookup)
        self.assertLess(second_lookup, allocate)
        self.assertLess(allocate, register)
        self.assertLess(register, ready)
        self.assertEqual(self.core.count("m_register(candidate->shader)"), 1)
        self.assertIn("++m_stats.raceLosers", self.core)
        self.assertIn("validateModule(existing, false, validate)", self.core)

    def test_failed_registration_retained_not_retried(self):
        self.assertIn("entry.state = State::FailedRegistration", self.core)
        self.assertIn("if (entry.state != State::Ready)", self.core)
        self.assertIn("same-key retry refused", self.core)
        self.assertNotIn("m_entries.erase", self.core)
        self.assertNotIn("m_entries.clear", self.core)

    def test_production_abi_order_and_admission_preserved(self):
        header = source("src/d3d9/d3d9_shader.h")
        block = header.split("class D3D9CommonShader {", 1)[1].split("/**", 1)[0]
        private = block.split("private:", 1)[1]
        expected = ["m_isgn", "m_usedSamplers", "m_usedRTs", "m_textureTypes", "m_info", "m_meta",
                    "m_constants", "m_maxDefinedFloatConst", "m_maxDefinedIntConst", "m_maxDefinedBoolConst", "m_shader"]
        fields = re.findall(r"\b(m_\w+)\b", private)
        self.assertEqual(fields, expected)
        ctor = self.production.split("D3D9CommonShader::D3D9CommonShader(const", 1)[1].split("void D3D9ShaderModuleSet", 1)[0]
        self.assertNotIn("registerShader", ctor)
        self.assertIn("m_usedSamplers <<= FirstVSSamplerSlot", ctor)
        for check in ("CanSWVP()", "allowSm3InD3D8", "maxFloatConstantsPS", "isSM2XOrNewer",
                      "caps::MaxFloatConstantsVS", "caps::MaxOtherConstants", "majorVersion == 1 && minorVersion > 4"):
            self.assertIn(check, self.production)
        self.assertLess(self.production.index("Invalid use of PS bool constant"), self.production.index("cache->getOrCompile"))

    def test_host_scanner_and_profile_still_precede_publication(self):
        create = self.host.split("void D3D9Backend::createShader(", 1)[1].split("void D3D9Backend::createDeclaration", 1)[0]
        self.assertLess(create.index("d3d9tokens::Scan"), create.index("DxsoReader reader"))
        self.assertLess(create.index("DxsoReader reader"), create.index("->getOrCompile"))
        self.assertIn("analysis.bytecodeByteLength == scan.bytes", create)
        for check in ("result.meta.maxConstIndexF <= FloatRegisters", "result.maxDefinedInt == -1",
                      "result.maxDefinedBool == -1", "result.usedRTs == 1", "result.usedSamplers <="):
            self.assertIn(check, create)
        self.assertNotIn("registerShader", create)
        self.assertIn("++m_stats.shadersCompiled", create)  # frozen legacy CREATE counter

    def test_cold_account_is_separate_and_lifetime_bounded(self):
        self.assertIn("diagnosticShaderCache() const", self.host)
        self.assertIn("result.vertexCookies[i] = DxvkShader::getCookie", self.host)
        self.assertIn("result.keyCapacityBytes = source.keyCapacityBytes", self.host)
        self.assertIn("result.constantCapacityBytes = source.constantCapacityBytes", self.host)
        shutdown = self.host.split("void shutdown(Stats& stats)", 1)[1].split("D3D9Backend::D3D9Backend", 1)[0]
        self.assertLess(shutdown.index("shaderModules.reset()"), shutdown.index("device = nullptr"))

    def test_native_coverage_uses_real_cpu_compiler(self):
        native = source("AutoTest/test_dxso_shader_module_cache.cpp")
        self.assertIn("dxso_shader_module_cache.h", native)
        for proof in ("race(true)", "race(false)", "registrationAttempts == 1", "weak.expired()",
                      "stats.compilesSucceeded == 2", 'GetModuleHandleW(L"vulkan-1.dll")'):
            self.assertIn(proof, native)
        self.assertNotIn("new DxvkInstance", native)


if __name__ == "__main__":
    unittest.main()
