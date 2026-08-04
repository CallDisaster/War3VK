#include "war3_curve_runtime.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace dxvk::war3::math {
namespace {

constexpr size_t kMaximumPrograms = 256u;
constexpr size_t kMaximumCurves = 512u;
constexpr float kPi = 3.14159265358979323846f;
constexpr float kEpsilon = 1.0e-6f;

Vec3 Add(const Vec3& a, const Vec3& b) {
  return {a.x + b.x, a.y + b.y, a.z + b.z};
}

Vec3 Subtract(const Vec3& a, const Vec3& b) {
  return {a.x - b.x, a.y - b.y, a.z - b.z};
}

Vec3 Scale(const Vec3& value, float scale) {
  return {value.x * scale, value.y * scale, value.z * scale};
}

float Dot(const Vec3& a, const Vec3& b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 Cross(const Vec3& a, const Vec3& b) {
  return {
      a.y * b.z - a.z * b.y,
      a.z * b.x - a.x * b.z,
      a.x * b.y - a.y * b.x,
  };
}

float Length(const Vec3& value) {
  return std::sqrt(std::max(0.0f, Dot(value, value)));
}

Vec3 NormalizeOr(const Vec3& value, const Vec3& fallback) {
  const float length = Length(value);
  return length > kEpsilon ? Scale(value, 1.0f / length) : fallback;
}

Vec3 Lerp(const Vec3& a, const Vec3& b, float t) {
  return Add(a, Scale(Subtract(b, a), t));
}

bool IsFinite(const Vec3& value) {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}

float SmoothStep01(float value) {
  const float t = std::clamp(value, 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

float EndpointLockMask(float t, bool lockStart, bool lockEnd) {
  const float clamped = std::clamp(t, 0.0f, 1.0f);
  if (lockStart && lockEnd)
    return std::sin(clamped * kPi);
  if (lockStart)
    return SmoothStep01(clamped);
  if (lockEnd)
    return SmoothStep01(1.0f - clamped);
  return 1.0f;
}

template <typename Records>
int32_t NextAvailableId(int32_t& nextId, size_t currentSize,
                        const Records& records) {
  const size_t attempts = currentSize + 1u;
  for (size_t index = 0u; index < attempts; ++index) {
    const int32_t candidate = nextId;
    if (nextId == std::numeric_limits<int32_t>::max())
      nextId = 1;
    else
      ++nextId;
    if (candidate > 0 && records.find(candidate) == records.end())
      return candidate;
  }
  return 0;
}

bool EvaluateSnapshotComponent(const CurveSnapshot& curve,
                               const CurveContext& context,
                               int32_t component,
                               float& output) {
  if (component < 0 || component > 2)
    return false;
  if (!curve.valid())
    return false;

  if (curve.isFormula() && curve.program->resultType() == ValueType::Scalar) {
    if (component != 0)
      return false;
    Value value;
    if (!EvaluateCurveRaw(curve, context, value))
      return false;
    output = value.x;
    return std::isfinite(output);
  }

  Vec3 point;
  if (!EvaluateCurveWorld(curve, context, point))
    return false;
  output = component == 0 ? point.x : component == 1 ? point.y : point.z;
  return std::isfinite(output);
}

} // namespace

CurveRuntime& CurveRuntime::instance() {
  static CurveRuntime runtime;
  return runtime;
}

int32_t CurveRuntime::compileProgram(const std::string& expression) {
  const CompileResult compiled = CompileExpression(expression);
  if (!compiled.ok()) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_lastCompileError = compiled.error + " at byte " +
        std::to_string(compiled.errorOffset);
    return 0;
  }

  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_programs.size() >= kMaximumPrograms) {
    m_lastCompileError = "program registry reached the 256-program limit";
    return 0;
  }
  const int32_t id = NextAvailableId(
      m_nextProgramId, m_programs.size(), m_programs);
  if (id <= 0 || !m_programs.emplace(id, compiled.program).second) {
    m_lastCompileError = "program id allocation failed";
    return 0;
  }
  m_lastCompileError.clear();
  return id;
}

bool CurveRuntime::destroyProgram(int32_t programId) {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_programs.erase(programId) != 0u;
}

bool CurveRuntime::isProgramAlive(int32_t programId) const {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_programs.find(programId) != m_programs.end();
}

std::string CurveRuntime::lastCompileError() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_lastCompileError;
}

int32_t CurveRuntime::createCurve(int32_t programId) {
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto program = m_programs.find(programId);
  if (program == m_programs.end() || m_curves.size() >= kMaximumCurves)
    return 0;
  const int32_t id = NextAvailableId(
      m_nextCurveId, m_curves.size(), m_curves);
  if (id <= 0)
    return 0;
  CurveRecord record;
  record.id = id;
  record.snapshot.program = program->second;
  return m_curves.emplace(id, std::move(record)).second ? id : 0;
}

int32_t CurveRuntime::createPointCurve(uint32_t expectedPointCount) {
  if (expectedPointCount < kMinimumPointCurvePoints ||
      expectedPointCount > kMaximumPointCurvePoints)
    return 0;

  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_curves.size() >= kMaximumCurves)
    return 0;
  const int32_t id = NextAvailableId(
      m_nextCurveId, m_curves.size(), m_curves);
  if (id <= 0)
    return 0;

  CurveRecord record;
  record.id = id;
  record.expectedPointCount = expectedPointCount;
  record.pointBuilder.reserve(expectedPointCount);
  return m_curves.emplace(id, std::move(record)).second ? id : 0;
}

bool CurveRuntime::appendPointCurve(int32_t curveId,
                                    const Vec3* points,
                                    uint32_t pointCount) {
  if (points == nullptr || pointCount == 0u || pointCount > 4u)
    return false;
  for (uint32_t index = 0u; index < pointCount; ++index) {
    if (!IsFinite(points[index]))
      return false;
  }

  std::lock_guard<std::mutex> lock(m_mutex);
  const auto curve = m_curves.find(curveId);
  if (curve == m_curves.end() || curve->second.expectedPointCount == 0u ||
      curve->second.snapshot.valid() ||
      curve->second.pointBuilder.size() + pointCount >
          curve->second.expectedPointCount)
    return false;
  curve->second.pointBuilder.insert(
      curve->second.pointBuilder.end(), points, points + pointCount);
  return true;
}

bool CurveRuntime::finalizePointCurve(int32_t curveId) {
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto curve = m_curves.find(curveId);
  if (curve == m_curves.end() || curve->second.expectedPointCount == 0u ||
      curve->second.snapshot.valid() ||
      curve->second.pointBuilder.size() != curve->second.expectedPointCount)
    return false;

  auto data = std::make_shared<PointCurveData>();
  data->points = curve->second.pointBuilder;
  data->cumulativeLengths.resize(data->points.size(), 0.0f);
  double total = 0.0;
  for (size_t index = 1u; index < data->points.size(); ++index) {
    total += double(Length(Subtract(data->points[index],
                                   data->points[index - 1u])));
    if (!std::isfinite(total) ||
        total > double(std::numeric_limits<float>::max()))
      return false;
    data->cumulativeLengths[index] = static_cast<float>(total);
  }
  if (total <= double(kEpsilon))
    return false;
  data->totalLength = static_cast<float>(total);

  curve->second.snapshot.pointCurve = std::move(data);
  curve->second.pointBuilder.clear();
  curve->second.pointBuilder.shrink_to_fit();
  return true;
}

bool CurveRuntime::destroyCurve(int32_t curveId) {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_curves.erase(curveId) != 0u;
}

bool CurveRuntime::isCurveAlive(int32_t curveId) const {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_curves.find(curveId) != m_curves.end();
}

bool CurveRuntime::setCurveReal(int32_t curveId,
                                const std::string& name,
                                float value) {
  if (!std::isfinite(value))
    return false;
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto curve = m_curves.find(curveId);
  if (curve == m_curves.end() || curve->second.snapshot.program == nullptr)
    return false;
  const int32_t parameter = curve->second.snapshot.program->findParameter(name);
  if (parameter < 0 ||
      static_cast<size_t>(parameter) >=
          curve->second.snapshot.parameters.size())
    return false;
  curve->second.snapshot.parameters[static_cast<size_t>(parameter)] = value;
  return true;
}

bool CurveRuntime::setCurveCoordinateMode(int32_t curveId, int32_t mode) {
  if (mode < static_cast<int32_t>(CurveCoordinateMode::Offset) ||
      mode > static_cast<int32_t>(CurveCoordinateMode::World))
    return false;
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto curve = m_curves.find(curveId);
  if (curve == m_curves.end() || curve->second.snapshot.program == nullptr)
    return false;
  const auto coordinateMode = static_cast<CurveCoordinateMode>(mode);
  const ValueType resultType = curve->second.snapshot.program->resultType();
  if ((coordinateMode == CurveCoordinateMode::Offset &&
       resultType != ValueType::Vec2) ||
      (coordinateMode != CurveCoordinateMode::Offset &&
       resultType != ValueType::Vec3))
    return false;
  curve->second.snapshot.coordinateMode = coordinateMode;
  return true;
}

bool CurveRuntime::setCurveEndpointLocks(int32_t curveId,
                                         bool lockStart,
                                         bool lockEnd) {
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto curve = m_curves.find(curveId);
  if (curve == m_curves.end() || !curve->second.snapshot.isFormula())
    return false;
  curve->second.snapshot.lockStart = lockStart;
  curve->second.snapshot.lockEnd = lockEnd;
  return true;
}

bool CurveRuntime::snapshotCurve(int32_t curveId,
                                 CurveSnapshot& output) const {
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto curve = m_curves.find(curveId);
  if (curve == m_curves.end() || !curve->second.snapshot.valid())
    return false;
  output = curve->second.snapshot;
  return true;
}

bool CurveRuntime::evaluateComponent(int32_t curveId,
                                     const CurveContext& context,
                                     int32_t component,
                                     float& output) const {
  CurveSnapshot curve;
  if (!snapshotCurve(curveId, curve))
    return false;
  return EvaluateSnapshotComponent(curve, context, component, output);
}

bool CurveRuntime::evaluateDerivativeComponent(
    int32_t curveId, const CurveContext& context,
    int32_t component, float& output) const {
  CurveSnapshot curve;
  if (!snapshotCurve(curveId, curve))
    return false;

  const uint32_t segments = std::clamp(context.segments, 2u, 256u);
  const float step = std::clamp(1.0f / (float(segments) * 4.0f),
                                1.0e-4f, 0.01f);
  CurveContext before = context;
  CurveContext after = context;
  before.t = std::max(0.0f, context.t - step);
  after.t = std::min(1.0f, context.t + step);
  if (after.t - before.t <= kEpsilon)
    return false;
  before.index = static_cast<uint32_t>(
      std::lround(before.t * float(segments)));
  after.index = static_cast<uint32_t>(
      std::lround(after.t * float(segments)));
  float first = 0.0f;
  float second = 0.0f;
  if (!EvaluateSnapshotComponent(curve, before, component, first) ||
      !EvaluateSnapshotComponent(curve, after, component, second))
    return false;
  output = (second - first) / (after.t - before.t);
  return std::isfinite(output);
}

bool CurveRuntime::evaluateArcLength(int32_t curveId,
                                     const CurveContext& context,
                                     uint32_t samples,
                                     float& output) const {
  if (samples < 2u || samples > 256u)
    return false;
  CurveSnapshot curve;
  if (!snapshotCurve(curveId, curve) || !curve.renderable())
    return false;

  if (curve.isPointCurve()) {
    output = curve.pointCurve->totalLength;
    return true;
  }

  CurveContext sampleContext = context;
  sampleContext.segments = samples;
  sampleContext.t = 0.0f;
  sampleContext.index = 0u;
  Vec3 previous;
  if (!EvaluateCurveWorld(curve, sampleContext, previous))
    return false;

  double total = 0.0;
  for (uint32_t index = 1u; index <= samples; ++index) {
    sampleContext.index = index;
    sampleContext.t = float(index) / float(samples);
    Vec3 current;
    if (!EvaluateCurveWorld(curve, sampleContext, current))
      return false;
    total += double(Length(Subtract(current, previous)));
    if (!std::isfinite(total) || total > double(std::numeric_limits<float>::max()))
      return false;
    previous = current;
  }
  output = static_cast<float>(total);
  return std::isfinite(output);
}

void CurveRuntime::reset() {
  std::lock_guard<std::mutex> lock(m_mutex);
  m_programs.clear();
  m_curves.clear();
  m_lastCompileError.clear();
  m_nextProgramId = 1;
  m_nextCurveId = 1;
}

bool EvaluateCurveRaw(const CurveSnapshot& curve,
                      const CurveContext& context,
                      Value& output,
                      EvaluationContext* resolvedContext) {
  if (curve.program == nullptr || !IsFinite(context.start) ||
      !IsFinite(context.end) || !std::isfinite(context.t) ||
      !std::isfinite(context.time))
    return false;

  EvaluationContext evaluation;
  evaluation.t = std::clamp(context.t, 0.0f, 1.0f);
  evaluation.time = context.time;
  evaluation.start = context.start;
  evaluation.end = context.end;
  evaluation.center = Scale(Add(context.start, context.end), 0.5f);
  const Vec3 delta = Subtract(context.end, context.start);
  evaluation.length = Length(delta);
  evaluation.forward = NormalizeOr(delta, {1.0f, 0.0f, 0.0f});
  evaluation.direction = evaluation.forward;
  const Vec3 reference = std::abs(evaluation.forward.z) < 0.99f
      ? Vec3{0.0f, 0.0f, 1.0f} : Vec3{0.0f, 1.0f, 0.0f};
  evaluation.right = NormalizeOr(
      Cross(reference, evaluation.forward), {0.0f, 1.0f, 0.0f});
  evaluation.up = NormalizeOr(
      Cross(evaluation.forward, evaluation.right), {0.0f, 0.0f, 1.0f});
  evaluation.index = float(context.index);
  evaluation.segments = float(std::max(context.segments, 1u));
  evaluation.seed = context.seed;
  evaluation.branchIndex = float(context.branchIndex);
  evaluation.branchDepth = float(context.branchDepth);
  if (resolvedContext != nullptr)
    *resolvedContext = evaluation;
  return curve.program->evaluate(evaluation, curve.parameters, output);
}

bool EvaluateCurveWorld(const CurveSnapshot& curve,
                        const CurveContext& context,
                        Vec3& output) {
  if (curve.isPointCurve()) {
    if (!curve.renderable() || !std::isfinite(context.t))
      return false;
    const PointCurveData& data = *curve.pointCurve;
    const float target = std::clamp(context.t, 0.0f, 1.0f) * data.totalLength;
    const auto upper = std::lower_bound(
        data.cumulativeLengths.begin(), data.cumulativeLengths.end(), target);
    if (upper == data.cumulativeLengths.begin()) {
      output = data.points.front();
      return true;
    }
    if (upper == data.cumulativeLengths.end()) {
      output = data.points.back();
      return true;
    }
    const size_t upperIndex = size_t(upper - data.cumulativeLengths.begin());
    const size_t lowerIndex = upperIndex - 1u;
    const float lowerLength = data.cumulativeLengths[lowerIndex];
    const float span = *upper - lowerLength;
    const float localT = span > kEpsilon
        ? std::clamp((target - lowerLength) / span, 0.0f, 1.0f)
        : 0.0f;
    output = Lerp(data.points[lowerIndex], data.points[upperIndex], localT);
    return IsFinite(output);
  }

  Value value;
  EvaluationContext evaluation;
  if (!EvaluateCurveRaw(curve, context, value, &evaluation))
    return false;

  const Vec3 base = Lerp(evaluation.start, evaluation.end, evaluation.t);
  const float mask = EndpointLockMask(
      evaluation.t, curve.lockStart, curve.lockEnd);
  switch (curve.coordinateMode) {
    case CurveCoordinateMode::Offset:
      if (value.type != ValueType::Vec2)
        return false;
      output = Add(base, Add(
          Scale(evaluation.right, value.x * mask),
          Scale(evaluation.up, value.y * mask)));
      break;
    case CurveCoordinateMode::Local: {
      if (value.type != ValueType::Vec3)
        return false;
      const Vec3 candidate = Add(evaluation.start, Add(
          Scale(evaluation.forward, value.x), Add(
              Scale(evaluation.right, value.y),
              Scale(evaluation.up, value.z))));
      output = Lerp(base, candidate, mask);
      break;
    }
    case CurveCoordinateMode::World: {
      if (value.type != ValueType::Vec3)
        return false;
      const Vec3 candidate{value.x, value.y, value.z};
      output = Lerp(base, candidate, mask);
      break;
    }
  }
  return IsFinite(output);
}

} // namespace dxvk::war3::math
