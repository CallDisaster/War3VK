#include "../war3_curve_runtime.h"

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace dxvk::war3::math;

bool Near(float actual, float expected, float tolerance = 1.0e-3f) {
  return std::abs(actual - expected) <= tolerance;
}

bool Require(bool condition, const char* message) {
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  return condition;
}

bool EvaluateScalarExpression(const char* expression, float expected,
                              float tolerance = 1.0e-3f) {
  const CompileResult compiled = CompileExpression(expression);
  if (!compiled.ok()) {
    std::cerr << "FAIL: compile '" << expression << "': "
              << compiled.error << '\n';
    return false;
  }
  EvaluationContext context;
  std::array<float, kMaximumExpressionParameters> parameters = {};
  Value value;
  if (!compiled.program->evaluate(context, parameters, value) ||
      value.type != ValueType::Scalar ||
      !Near(value.x, expected, tolerance)) {
    std::cerr << "FAIL: evaluate '" << expression << "' expected "
              << expected << " got " << value.x << '\n';
    return false;
  }
  return true;
}

} // namespace

int main() {
  CurveRuntime& runtime = CurveRuntime::instance();
  runtime.reset();

  struct ScalarCase {
    const char* expression;
    float expected;
  };
  const std::vector<ScalarCase> scalarCases = {
      {"sin(pi/2)", 1.0f}, {"cos(0)", 1.0f}, {"tan(0)", 0.0f},
      {"asin(1)", 1.5707963f}, {"acos(1)", 0.0f},
      {"atan(1)", 0.7853982f}, {"atan2(1,1)", 0.7853982f},
      {"sqrt(9)", 3.0f}, {"pow(2,3)", 8.0f}, {"exp(0)", 1.0f},
      {"log(exp(1))", 1.0f}, {"abs(-2)", 2.0f}, {"sign(-2)", -1.0f},
      {"floor(1.9)", 1.0f}, {"ceil(1.1)", 2.0f},
      {"round(1.6)", 2.0f}, {"fract(1.25)", 0.25f},
      {"min(2,3)", 2.0f}, {"max(2,3)", 3.0f},
      {"clamp(4,1,3)", 3.0f}, {"saturate(2)", 1.0f},
      {"lerp(2,4,0.5)", 3.0f}, {"inverseLerp(2,4,3)", 0.5f},
      {"remap(0,10,5,10,20)", 15.0f}, {"step(0.5,0.4)", 0.0f},
      {"smoothstep(0,1,0.5)", 0.5f},
      {"smootherstep(0,1,0.5)", 0.5f},
      {"dot(vec3(1,2,3),vec3(4,5,6))", 32.0f},
      {"z(cross(vec3(1,0,0),vec3(0,1,0)))", 1.0f},
      {"length(vec2(3,4))", 5.0f},
      {"distance(vec2(0,0),vec2(3,4))", 5.0f},
      {"x(normalize(vec2(3,4)))", 0.6f},
      {"x(project(vec2(3,4),vec2(1,0)))", 3.0f},
      {"y(reject(vec2(3,4),vec2(1,0)))", 4.0f},
      {"y(rotateAroundAxis(vec3(1,0,0),vec3(0,0,1),pi/2))", 1.0f},
      {"endpointMask(0.5)", 1.0f},
      {"endpointMask(0.5,0.25,0.25)", 1.0f},
      {"repeat(7,3)", 1.0f}, {"pingpong(4,3)", 2.0f},
      {"bezier2(0,2,4,0.5)", 2.0f},
      {"bezier3(0,0,4,4,0.5)", 2.0f},
  };
  for (const ScalarCase& test : scalarCases) {
    if (!EvaluateScalarExpression(test.expression, test.expected))
      return 1;
  }

  const int32_t helixProgram = runtime.compileProgram(
      "vec2(cos(t*turns*tau+time*speed)*radius,"
      "sin(t*turns*tau+time*speed)*radius)");
  if (!Require(helixProgram > 0, "compile offset helix"))
    return 1;
  const int32_t helix = runtime.createCurve(helixProgram);
  if (!Require(helix > 0, "create offset curve") ||
      !Require(runtime.setCurveReal(helix, "turns", 1.0f), "set turns") ||
      !Require(runtime.setCurveReal(helix, "speed", 0.0f), "set speed") ||
      !Require(runtime.setCurveReal(helix, "radius", 10.0f), "set radius"))
    return 1;

  CurveContext context;
  context.start = {0.0f, 0.0f, 0.0f};
  context.end = {100.0f, 0.0f, 0.0f};
  context.segments = 64u;
  context.t = 0.5f;
  context.index = 32u;
  CurveSnapshot snapshot;
  Vec3 point;
  if (!Require(runtime.snapshotCurve(helix, snapshot), "snapshot curve") ||
      !Require(snapshot.renderable(), "offset vec2 snapshot is renderable") ||
      !Require(!runtime.setCurveCoordinateMode(
                   helix, static_cast<int32_t>(CurveCoordinateMode::Local)),
               "reject vec2 in local coordinate mode") ||
      !Require(EvaluateCurveWorld(snapshot, context, point), "evaluate helix") ||
      !Require(Near(point.x, 50.0f) && Near(point.y, -10.0f) &&
                   Near(point.z, 0.0f), "offset basis and midpoint"))
    return 1;

  context.t = 0.0f;
  context.index = 0u;
  if (!Require(EvaluateCurveWorld(snapshot, context, point), "evaluate start") ||
      !Require(Near(point.x, 0.0f) && Near(point.y, 0.0f) &&
                   Near(point.z, 0.0f), "start endpoint lock"))
    return 1;
  context.t = 1.0f;
  context.index = 64u;
  if (!Require(EvaluateCurveWorld(snapshot, context, point), "evaluate end") ||
      !Require(Near(point.x, 100.0f) && Near(point.y, 0.0f) &&
                   Near(point.z, 0.0f), "end endpoint lock"))
    return 1;

  const int32_t lineProgram = runtime.compileProgram("vec3(t*length,0,0)");
  const int32_t line = runtime.createCurve(lineProgram);
  if (!Require(lineProgram > 0 && line > 0, "create local line") ||
      !Require(runtime.setCurveCoordinateMode(
                   line, static_cast<int32_t>(CurveCoordinateMode::Local)),
               "set local coordinate mode"))
    return 1;
  context.t = 0.4f;
  context.index = 26u;
  float derivative = 0.0f;
  if (!Require(runtime.evaluateDerivativeComponent(
                   line, context, 0, derivative), "finite difference") ||
      !Require(Near(derivative, 100.0f, 0.02f), "line derivative") ||
      !Require(runtime.evaluateArcLength(line, context, 32u, derivative),
               "arc length") ||
      !Require(Near(derivative, 100.0f, 0.02f), "line arc length"))
    return 1;

  const int32_t pointCurve = runtime.createPointCurve(5u);
  const std::array<Vec3, 5u> authoredPoints = {{
      {0.0f, 0.0f, 0.0f},
      {10.0f, 0.0f, 0.0f},
      {10.0f, 10.0f, 0.0f},
      {20.0f, 10.0f, 0.0f},
      {20.0f, 20.0f, 0.0f},
  }};
  if (!Require(pointCurve > 0, "create bounded point curve") ||
      !Require(!runtime.finalizePointCurve(pointCurve),
               "reject incomplete point curve") ||
      !Require(runtime.appendPointCurve(
                   pointCurve, authoredPoints.data(), 4u),
               "append four-point transport chunk") ||
      !Require(runtime.appendPointCurve(
                   pointCurve, authoredPoints.data() + 4u, 1u),
               "append final partial transport chunk") ||
      !Require(runtime.finalizePointCurve(pointCurve),
               "atomically finalize point curve") ||
      !Require(!runtime.appendPointCurve(
                   pointCurve, authoredPoints.data(), 1u),
               "frozen point curve rejects later append"))
    return 1;

  CurveSnapshot pointSnapshot;
  context.t = 0.625f;
  if (!Require(runtime.snapshotCurve(pointCurve, pointSnapshot),
               "snapshot finalized point curve") ||
      !Require(pointSnapshot.isPointCurve() && pointSnapshot.renderable(),
               "point curve snapshot is renderable") ||
      !Require(EvaluateCurveWorld(pointSnapshot, context, point),
               "evaluate point curve by normalized arc length") ||
      !Require(Near(point.x, 15.0f) && Near(point.y, 10.0f),
               "point curve interpolation uses cumulative arc length") ||
      !Require(runtime.evaluateArcLength(pointCurve, context, 2u, derivative),
               "point curve exposes baked arc length") ||
      !Require(Near(derivative, 40.0f), "point curve baked arc length value") ||
      !Require(runtime.createPointCurve(1u) == 0,
               "reject point curve below minimum") ||
      !Require(runtime.createPointCurve(kMaximumPointCurvePoints + 1u) == 0,
               "reject point curve above hard maximum"))
    return 1;

  const int32_t noiseProgram = runtime.compileProgram("noise1(t*8,seed)");
  const int32_t noiseCurve = runtime.createCurve(noiseProgram);
  float noiseA = 0.0f;
  float noiseB = 0.0f;
  context.t = 0.37f;
  context.seed = 12345u;
  if (!Require(noiseProgram > 0 && noiseCurve > 0, "compile noise") ||
      !Require(runtime.evaluateComponent(
                   noiseCurve, context, 0, noiseA), "evaluate noise A") ||
      !Require(runtime.evaluateComponent(
                   noiseCurve, context, 0, noiseB), "evaluate noise B") ||
      !Require(noiseA == noiseB, "deterministic noise"))
    return 1;

  if (!Require(runtime.destroyProgram(helixProgram), "destroy program handle") ||
      !Require(!runtime.isProgramAlive(helixProgram), "program handle dead") ||
      !Require(EvaluateCurveWorld(snapshot, context, point),
               "immutable curve snapshot retains bytecode"))
    return 1;

  if (!Require(runtime.compileProgram("") == 0, "reject empty expression") ||
      !Require(!runtime.lastCompileError().empty(), "compile error exposed") ||
      !Require(runtime.compileProgram("vec3(t,0,unknownFn(t))") == 0,
               "reject unknown function") ||
      !Require(runtime.compileProgram(std::string(385u, '1')) == 0,
               "reject oversized expression"))
    return 1;

  const int32_t invalidProgram = runtime.compileProgram("1/(t-t)");
  const int32_t invalidCurve = runtime.createCurve(invalidProgram);
  float invalidValue = 0.0f;
  if (!Require(invalidProgram > 0 && invalidCurve > 0,
               "runtime-invalid expression compiles") ||
      !Require(!runtime.evaluateComponent(
                   invalidCurve, context, 0, invalidValue),
               "division by zero fails closed"))
    return 1;

  runtime.reset();
  std::cout << "war3_math_curve_runtime_test: PASS\n";
  return 0;
}
