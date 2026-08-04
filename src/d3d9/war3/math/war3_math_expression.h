#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace dxvk::war3::math {

constexpr size_t kMaximumExpressionBytes = 384u;
constexpr size_t kMaximumExpressionParameters = 16u;
constexpr size_t kMaximumExpressionInstructions = 256u;
constexpr size_t kMaximumExpressionStack = 64u;

struct Vec3 {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
};

enum class ValueType : uint8_t {
  Invalid = 0,
  Scalar,
  Vec2,
  Vec3,
};

struct Value {
  ValueType type = ValueType::Invalid;
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;

  static Value scalar(float value);
  static Value vec2(float x, float y);
  static Value vec3(float x, float y, float z);
};

struct EvaluationContext {
  float t = 0.0f;
  float time = 0.0f;
  float length = 0.0f;
  float index = 0.0f;
  float segments = 1.0f;
  uint32_t seed = 0u;
  float branchIndex = 0.0f;
  float branchDepth = 0.0f;
  Vec3 start = {};
  Vec3 end = {};
  Vec3 center = {};
  Vec3 direction = {1.0f, 0.0f, 0.0f};
  Vec3 forward = {1.0f, 0.0f, 0.0f};
  Vec3 right = {0.0f, 1.0f, 0.0f};
  Vec3 up = {0.0f, 0.0f, 1.0f};
};

enum class OpCode : uint8_t {
  PushConstant,
  PushVariable,
  PushParameter,
  Add,
  Subtract,
  Multiply,
  Divide,
  Negate,
  Call,
};

enum class VariableId : uint8_t {
  T,
  Time,
  Length,
  Index,
  Segments,
  Seed,
  BranchIndex,
  BranchDepth,
  Start,
  End,
  Center,
  Direction,
  Forward,
  Right,
  Up,
};

enum class FunctionId : uint8_t {
  Vec2,
  Vec3,
  X,
  Y,
  Z,
  Sin,
  Cos,
  Tan,
  Asin,
  Acos,
  Atan,
  Atan2,
  Sqrt,
  Pow,
  Exp,
  Log,
  Abs,
  Sign,
  Floor,
  Ceil,
  Round,
  Fract,
  Min,
  Max,
  Clamp,
  Saturate,
  Lerp,
  InverseLerp,
  Remap,
  Step,
  SmoothStep,
  SmootherStep,
  Dot,
  Cross,
  VectorLength,
  Distance,
  Normalize,
  Project,
  Reject,
  RotateAroundAxis,
  EndpointMask,
  Noise1,
  Repeat,
  PingPong,
  Bezier2,
  Bezier3,
};

struct Instruction {
  OpCode op = OpCode::PushConstant;
  ValueType outputType = ValueType::Invalid;
  uint16_t operand = 0u;
  uint8_t argumentCount = 0u;
  float constant = 0.0f;
};

struct CompileResult;

class Program {
public:
  ValueType resultType() const;
  size_t parameterCount() const;
  const std::string& parameterName(size_t index) const;
  int32_t findParameter(std::string_view name) const;
  size_t instructionCount() const;

  bool evaluate(
      const EvaluationContext& context,
      const std::array<float, kMaximumExpressionParameters>& parameters,
      Value& output) const;

private:
  friend struct CompileResult;
  friend CompileResult CompileExpression(std::string_view expression);

  ValueType m_resultType = ValueType::Invalid;
  std::vector<std::string> m_parameterNames;
  std::vector<Instruction> m_instructions;
};

struct CompileResult {
  std::shared_ptr<const Program> program;
  std::string error;
  size_t errorOffset = 0u;

  bool ok() const {
    return program != nullptr;
  }
};

CompileResult CompileExpression(std::string_view expression);

} // namespace dxvk::war3::math
