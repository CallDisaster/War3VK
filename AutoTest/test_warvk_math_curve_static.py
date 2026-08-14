import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MATH_HEADER = ROOT / "src/d3d9/war3/math/war3_math_expression.h"
MATH_SOURCE = ROOT / "src/d3d9/war3/math/war3_math_expression.cpp"
CURVE_HEADER = ROOT / "src/d3d9/war3/math/war3_curve_runtime.h"
CURVE_SOURCE = ROOT / "src/d3d9/war3/math/war3_curve_runtime.cpp"
CURVE_TEST = ROOT / "src/d3d9/war3/math/tests/war3_math_curve_runtime_test.cpp"
LIGHTNING_HEADER = ROOT / "src/d3d9/war3/render/war3_lightning_runtime.h"
LIGHTNING_SOURCE = ROOT / "src/d3d9/war3/render/war3_lightning_runtime.cpp"
JAPI_SOURCE = ROOT / "src/d3d9/war3/japi/war3_japi_v1.cpp"
JASS_BRIDGE = ROOT / "src/d3d9/war3/hooks/war3_jass_command_bridge.cpp"
RUNTIME_BOOTSTRAP = ROOT / "src/d3d9/war3/platform/war3_runtime_bootstrap.cpp"
MESON = ROOT / "src/d3d9/meson.build"
JASS_API = ROOT / "WarVK/jass/warvk_api.j"
JASS_CONSTANTS = ROOT / "WarVK/jass/warvk_constant.j"
SMOKE = ROOT / "WarVK/jass/warvk_smoke_test.j"
README = ROOT / "WarVK/README.md"
MATH_DOC = ROOT / "WarVK/MATH_CURVE_API.md"


class WarVKMathCurveStaticTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.math_h = MATH_HEADER.read_text(encoding="utf-8")
        cls.math_cpp = MATH_SOURCE.read_text(encoding="utf-8")
        cls.curve_h = CURVE_HEADER.read_text(encoding="utf-8")
        cls.curve_cpp = CURVE_SOURCE.read_text(encoding="utf-8")
        cls.lightning_h = LIGHTNING_HEADER.read_text(encoding="utf-8")
        cls.lightning_cpp = LIGHTNING_SOURCE.read_text(encoding="utf-8")
        cls.japi = JAPI_SOURCE.read_text(encoding="utf-8")
        cls.jass = JASS_API.read_text(encoding="utf-8")
        cls.constants = JASS_CONSTANTS.read_text(encoding="utf-8")
        cls.meson = MESON.read_text(encoding="utf-8")
        cls.doc = MATH_DOC.read_text(encoding="utf-8")

    def test_expression_vm_has_hard_resource_bounds(self):
        for contract in (
            "kMaximumExpressionBytes = 384u",
            "kMaximumExpressionParameters = 16u",
            "kMaximumExpressionInstructions = 256u",
            "kMaximumExpressionStack = 64u",
            "kMaximumParseDepth = 32u",
        ):
            self.assertIn(contract, self.math_h + self.math_cpp)
        self.assertIn("std::array<Value, kMaximumExpressionStack>", self.math_cpp)
        self.assertIn("expression exceeds 384 bytes", self.math_cpp)
        for forbidden_opcode in ("Jump", "Store", "CallUserFunction", "Allocate"):
            self.assertNotIn(forbidden_opcode, self.math_h)

    def test_phase_one_types_context_and_functions_are_compiled(self):
        for symbol in (
            "ValueType::Scalar",
            "ValueType::Vec2",
            "ValueType::Vec3",
            "VariableId::BranchIndex",
            "VariableId::BranchDepth",
            "FunctionId::RotateAroundAxis",
            "FunctionId::EndpointMask",
            "FunctionId::Noise1",
            "FunctionId::Bezier3",
        ):
            self.assertIn(symbol, self.math_cpp)
        self.assertIn("HashSigned", self.math_cpp)
        self.assertNotIn("std::rand", self.math_cpp)
        self.assertNotIn("rand()", self.math_cpp)

    def test_curve_runtime_uses_snapshots_and_three_coordinate_modes(self):
        for mode in ("Offset = 0", "Local = 1", "World = 2"):
            self.assertIn(mode, self.curve_h)
        self.assertIn("std::shared_ptr<const Program> program", self.curve_h)
        self.assertIn("bool renderable() const", self.curve_h)
        self.assertIn("CurveSnapshot snapshot", self.curve_h)
        self.assertIn("EndpointLockMask", self.curve_cpp)
        self.assertIn("evaluateDerivativeComponent", self.curve_cpp)
        self.assertIn("evaluateArcLength", self.curve_cpp)
        self.assertIn("samples > 256u", self.curve_cpp)
        self.assertIn("kMaximumPrograms = 256u", self.curve_cpp)
        self.assertIn("kMaximumCurves = 512u", self.curve_cpp)

    def test_lightning_renderer_consumes_formula_in_cpp(self):
        self.assertIn("math::CurveSnapshot formulaCurve", self.lightning_h)
        self.assertIn("setTemplateFormulaCurve", self.lightning_h)
        self.assertIn("setFormulaCurve", self.lightning_h)
        center = re.search(
            r"War3LightningPoint ResolveRibbonCenterPoint\(.*?\n\}",
            self.lightning_cpp,
            re.DOTALL,
        ).group(0)
        self.assertIn("record.formulaCurve.valid()", center)
        self.assertIn("math::EvaluateCurveWorld", center)
        self.assertLess(
            center.index("math::EvaluateCurveWorld"),
            center.index("ResolveFractalNoise"),
            "formula must take priority over the legacy centerline",
        )
        self.assertIn("context.branchIndex = seedSalt", center)
        self.assertIn("nowSec - record.createdSec", center)

    def test_japi_exposes_cpu_only_math_curve_contract(self):
        self.assertIn('kApiVersion = "WarVK JAPI 1.21.00"', self.japi)
        self.assertIn("kFeatureMathCurve = 0x00002000u", self.japi)
        for command in (
            "math.program.compile",
            "math.program.destroy",
            "math.program.isAlive",
            "math.program.lastError",
            "math.evaluateReal",
            "math.evaluateInteger",
            "curve.create",
            "curve.destroy",
            "curve.setReal",
            "curve.setCoordinateMode",
            "curve.setEndpointLocks",
            "curve.evaluateComponent",
            "curve.derivativeComponent",
            "curve.arcLength",
            "lightning.template.setFormulaCurve",
            "lightning.setFormulaCurve",
        ):
            self.assertIn(f'"{command}"', self.japi)
        direct_settings_policy = self.japi.split(
            "bool CommandUsesDirectRenderSettings", 1
        )[1].split("Reply DispatchBackend", 1)[0]
        self.assertNotIn("MathProgramCompile", direct_settings_policy)
        self.assertNotIn("CurvePointFinalize", direct_settings_policy)
        self.assertIn(
            "if (CommandUsesDirectRenderSettings(request.spec->id))",
            self.japi,
        )
        self.assertIn("math::CurveRuntime::instance().reset()", self.japi)
        bridge = JASS_BRIDGE.read_text(encoding="utf-8")
        self.assertIn("WarVK JAPI 1.21.00", bridge)
        self.assertNotIn("WarVK JAPI 1.2.0-math-curves", bridge)
        bootstrap = RUNTIME_BOOTSTRAP.read_text(encoding="utf-8")
        self.assertIn("math::CurveRuntime::instance().reset()", bootstrap)

    def test_jass_and_ydwe_surface_the_same_authoring_model(self):
        for symbol in (
            "WarVKCompileMathProgram",
            "WarVKCreateCurve",
            "WarVKSetCurveReal",
            "WarVKSetCurveCoordinateMode",
            "WarVKSetCurveEndpointLocks",
            "WarVKEvaluateMathReal",
            "WarVKEvaluateMathInteger",
            "WarVKEvaluateCurveComponent",
            "WarVKEvaluateCurveDerivativeComponent",
            "WarVKGetCurveArcLength",
            "WarVKSetLightningTemplateFormulaCurve",
            "WarVKSetLightningFormulaCurve",
        ):
            self.assertIn(f"function {symbol}", self.jass)
            self.assertIn(f'script = "{symbol}"',
                          (ROOT / "WarVK/action.txt").read_text(encoding="utf-8") +
                          (ROOT / "WarVK/call.txt").read_text(encoding="utf-8"))
        self.assertIn("WARVK_FEATURE_MATH_CURVE = 8192", self.constants)
        self.assertIn("WARVK_CURVE_COORDINATE_OFFSET = 0", self.constants)
        self.assertIn("WARVK_CURVE_COORDINATE_LOCAL = 1", self.constants)
        self.assertIn("WARVK_CURVE_COORDINATE_WORLD = 2", self.constants)

    def test_build_and_author_validation_assets_are_present(self):
        for source in (
            "war3/math/war3_math_expression.cpp",
            "war3/math/war3_curve_runtime.cpp",
            "war3/math/tests/war3_math_curve_runtime_test.cpp",
            "war3_math_curve_runtime_test",
        ):
            self.assertIn(source, self.meson)
        self.assertTrue(CURVE_TEST.is_file())
        smoke = SMOKE.read_text(encoding="utf-8")
        self.assertIn("WarVKBeginFormulaLightningSmokeTest", smoke)
        self.assertIn("WarVKSetLightningTemplateFormulaCurve", smoke)
        self.assertIn("WarVKDestroyMathProgram", smoke)

    def test_documentation_states_current_and_deferred_boundaries(self):
        self.assertIn("MathProgram", self.doc)
        self.assertIn("OFFSET", self.doc)
        self.assertIn("LOCAL", self.doc)
        self.assertIn("WORLD", self.doc)
        self.assertIn("不能包含协议分隔符 `;`", self.doc)
        self.assertIn("自动微分", self.doc)
        self.assertIn("RK4", self.doc)
        self.assertIn("暂未宣称实现", self.doc)
        self.assertIn("MATH_CURVE_API.md", README.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
