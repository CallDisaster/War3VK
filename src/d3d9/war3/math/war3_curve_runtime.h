#pragma once

#include "war3_math_expression.h"

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace dxvk::war3::math {

enum class CurveCoordinateMode : int32_t {
  Offset = 0,
  Local = 1,
  World = 2,
};

constexpr uint32_t kMinimumPointCurvePoints = 2u;
constexpr uint32_t kMaximumPointCurvePoints = 1024u;

// Immutable, arc-length-qualified point data.  JASS builds this through a
// bounded staging record and finalize publishes one shared snapshot to the
// renderer; render frames never observe a half-uploaded polyline.
struct PointCurveData {
  std::vector<Vec3> points;
  std::vector<float> cumulativeLengths;
  float totalLength = 0.0f;
};

struct CurveSnapshot {
  std::shared_ptr<const Program> program;
  std::shared_ptr<const PointCurveData> pointCurve;
  std::array<float, kMaximumExpressionParameters> parameters = {};
  CurveCoordinateMode coordinateMode = CurveCoordinateMode::Offset;
  bool lockStart = true;
  bool lockEnd = true;

  bool valid() const {
    return program != nullptr || pointCurve != nullptr;
  }

  bool isFormula() const {
    return program != nullptr && pointCurve == nullptr;
  }

  bool isPointCurve() const {
    return pointCurve != nullptr && program == nullptr;
  }

  bool renderable() const {
    if (pointCurve != nullptr)
      return program == nullptr &&
          pointCurve->points.size() >= kMinimumPointCurvePoints &&
          pointCurve->points.size() == pointCurve->cumulativeLengths.size() &&
          pointCurve->totalLength > 0.0f;
    if (program == nullptr)
      return false;
    return coordinateMode == CurveCoordinateMode::Offset
        ? program->resultType() == ValueType::Vec2
        : program->resultType() == ValueType::Vec3;
  }
};

struct CurveContext {
  Vec3 start = {};
  Vec3 end = {};
  float t = 0.0f;
  float time = 0.0f;
  uint32_t seed = 0u;
  uint32_t index = 0u;
  uint32_t segments = 1u;
  uint32_t branchIndex = 0u;
  uint32_t branchDepth = 0u;
};

class CurveRuntime {
public:
  static CurveRuntime& instance();

  int32_t compileProgram(const std::string& expression);
  bool destroyProgram(int32_t programId);
  bool isProgramAlive(int32_t programId) const;
  std::string lastCompileError() const;

  int32_t createCurve(int32_t programId);
  int32_t createPointCurve(uint32_t expectedPointCount);
  bool appendPointCurve(int32_t curveId,
                        const Vec3* points,
                        uint32_t pointCount);
  bool finalizePointCurve(int32_t curveId);
  bool destroyCurve(int32_t curveId);
  bool isCurveAlive(int32_t curveId) const;
  bool setCurveReal(int32_t curveId, const std::string& name, float value);
  bool setCurveCoordinateMode(int32_t curveId, int32_t mode);
  bool setCurveEndpointLocks(int32_t curveId,
                             bool lockStart,
                             bool lockEnd);
  bool snapshotCurve(int32_t curveId, CurveSnapshot& output) const;

  bool evaluateComponent(int32_t curveId,
                         const CurveContext& context,
                         int32_t component,
                         float& output) const;
  bool evaluateDerivativeComponent(int32_t curveId,
                                   const CurveContext& context,
                                   int32_t component,
                                   float& output) const;
  bool evaluateArcLength(int32_t curveId,
                         const CurveContext& context,
                         uint32_t samples,
                         float& output) const;

  void reset();

private:
  struct CurveRecord {
    int32_t id = 0;
    CurveSnapshot snapshot;
    uint32_t expectedPointCount = 0u;
    std::vector<Vec3> pointBuilder;
  };

  CurveRuntime() = default;

  mutable std::mutex m_mutex;
  int32_t m_nextProgramId = 1;
  int32_t m_nextCurveId = 1;
  std::unordered_map<int32_t, std::shared_ptr<const Program>> m_programs;
  std::unordered_map<int32_t, CurveRecord> m_curves;
  std::string m_lastCompileError;
};

bool EvaluateCurveRaw(const CurveSnapshot& curve,
                      const CurveContext& context,
                      Value& output,
                      EvaluationContext* resolvedContext = nullptr);
bool EvaluateCurveWorld(const CurveSnapshot& curve,
                        const CurveContext& context,
                        Vec3& output);

} // namespace dxvk::war3::math
