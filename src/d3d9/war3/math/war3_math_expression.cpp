#include "war3_math_expression.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <system_error>
#include <utility>

namespace dxvk::war3::math {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTau = 6.28318530717958647692f;
constexpr float kEpsilon = 1.0e-8f;
constexpr size_t kMaximumParseDepth = 32u;

size_t ComponentCount(ValueType type) {
  switch (type) {
    case ValueType::Scalar: return 1u;
    case ValueType::Vec2: return 2u;
    case ValueType::Vec3: return 3u;
    default: return 0u;
  }
}

bool IsVector(ValueType type) {
  return type == ValueType::Vec2 || type == ValueType::Vec3;
}

float GetComponent(const Value& value, size_t index) {
  if (value.type == ValueType::Scalar)
    return value.x;
  if (index == 0u)
    return value.x;
  if (index == 1u)
    return value.y;
  return value.z;
}

void SetComponent(Value& value, size_t index, float component) {
  if (index == 0u)
    value.x = component;
  else if (index == 1u)
    value.y = component;
  else
    value.z = component;
}

Value MakeValue(ValueType type) {
  Value result;
  result.type = type;
  return result;
}

bool IsFinite(const Value& value) {
  const size_t count = ComponentCount(value.type);
  if (count == 0u)
    return false;
  for (size_t index = 0u; index < count; ++index) {
    if (!std::isfinite(GetComponent(value, index)))
      return false;
  }
  return true;
}

float Clamp01(float value) {
  return std::clamp(value, 0.0f, 1.0f);
}

float SmoothStep01(float value) {
  const float t = Clamp01(value);
  return t * t * (3.0f - 2.0f * t);
}

float SmootherStep01(float value) {
  const float t = Clamp01(value);
  return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

uint32_t HashU32(uint32_t value) {
  value ^= value >> 16u;
  value *= 0x7feb352du;
  value ^= value >> 15u;
  value *= 0x846ca68bu;
  value ^= value >> 16u;
  return value;
}

float HashSigned(uint32_t seed, int32_t coordinate) {
  const uint32_t hash = HashU32(
      seed ^ (static_cast<uint32_t>(coordinate) * 0x9e3779b9u));
  return float(hash & 0x00ffffffu) / float(0x00ffffffu) * 2.0f - 1.0f;
}

float Noise1(float coordinate, uint32_t seed) {
  const float wrapped = std::fmod(coordinate, 1048576.0f);
  const float baseValue = std::floor(wrapped);
  const auto base = static_cast<int32_t>(baseValue);
  const float fraction = wrapped - baseValue;
  const float blend = SmoothStep01(fraction);
  const float first = HashSigned(seed, base);
  const float second = HashSigned(seed, base + 1);
  return first + (second - first) * blend;
}

uint32_t FloatSeed(float value, uint32_t fallback) {
  if (!std::isfinite(value))
    return fallback;
  const double wrapped = std::fmod(std::abs(double(value)), 4294967296.0);
  return static_cast<uint32_t>(wrapped);
}

bool IsIdentifierStart(char character) {
  return (character >= 'A' && character <= 'Z') ||
         (character >= 'a' && character <= 'z') || character == '_';
}

bool IsIdentifierContinue(char character) {
  return IsIdentifierStart(character) ||
         (character >= '0' && character <= '9');
}

bool IsName(std::string_view value, const char* expected) {
  return value == expected;
}

struct FunctionResolution {
  FunctionId id = FunctionId::Sin;
  ValueType output = ValueType::Invalid;
  bool valid = false;
};

bool AllSame(const std::vector<ValueType>& types, size_t first, size_t last) {
  if (first >= last || last > types.size())
    return false;
  for (size_t index = first + 1u; index < last; ++index) {
    if (types[index] != types[first])
      return false;
  }
  return true;
}

FunctionResolution ResolveFunction(
    std::string_view name, const std::vector<ValueType>& arguments) {
  FunctionResolution result;
  const size_t count = arguments.size();
  const auto unaryAny = [&](FunctionId id) {
    if (count == 1u && arguments[0] != ValueType::Invalid)
      result = {id, arguments[0], true};
  };
  const auto binarySame = [&](FunctionId id) {
    if (count == 2u && arguments[0] == arguments[1] &&
        arguments[0] != ValueType::Invalid)
      result = {id, arguments[0], true};
  };

  if (IsName(name, "vec2")) {
    if (count == 2u && arguments[0] == ValueType::Scalar &&
        arguments[1] == ValueType::Scalar)
      return {FunctionId::Vec2, ValueType::Vec2, true};
    return result;
  }
  if (IsName(name, "vec3")) {
    if (count == 3u && arguments[0] == ValueType::Scalar &&
        arguments[1] == ValueType::Scalar &&
        arguments[2] == ValueType::Scalar)
      return {FunctionId::Vec3, ValueType::Vec3, true};
    return result;
  }
  if (IsName(name, "x") || IsName(name, "y") || IsName(name, "z")) {
    if (count == 1u && IsVector(arguments[0]) &&
        (!IsName(name, "z") || arguments[0] == ValueType::Vec3)) {
      const FunctionId id = IsName(name, "x") ? FunctionId::X :
          IsName(name, "y") ? FunctionId::Y : FunctionId::Z;
      return {id, ValueType::Scalar, true};
    }
    return result;
  }

  if (IsName(name, "sin")) unaryAny(FunctionId::Sin);
  else if (IsName(name, "cos")) unaryAny(FunctionId::Cos);
  else if (IsName(name, "tan")) unaryAny(FunctionId::Tan);
  else if (IsName(name, "asin")) unaryAny(FunctionId::Asin);
  else if (IsName(name, "acos")) unaryAny(FunctionId::Acos);
  else if (IsName(name, "atan")) unaryAny(FunctionId::Atan);
  else if (IsName(name, "sqrt")) unaryAny(FunctionId::Sqrt);
  else if (IsName(name, "exp")) unaryAny(FunctionId::Exp);
  else if (IsName(name, "log")) unaryAny(FunctionId::Log);
  else if (IsName(name, "abs")) unaryAny(FunctionId::Abs);
  else if (IsName(name, "sign")) unaryAny(FunctionId::Sign);
  else if (IsName(name, "floor")) unaryAny(FunctionId::Floor);
  else if (IsName(name, "ceil")) unaryAny(FunctionId::Ceil);
  else if (IsName(name, "round")) unaryAny(FunctionId::Round);
  else if (IsName(name, "fract")) unaryAny(FunctionId::Fract);
  else if (IsName(name, "saturate")) unaryAny(FunctionId::Saturate);
  else if (IsName(name, "atan2")) binarySame(FunctionId::Atan2);
  else if (IsName(name, "pow")) binarySame(FunctionId::Pow);
  else if (IsName(name, "min")) binarySame(FunctionId::Min);
  else if (IsName(name, "max")) binarySame(FunctionId::Max);

  if (result.valid)
    return result;

  if (IsName(name, "clamp") && count == 3u && AllSame(arguments, 0u, 3u))
    return {FunctionId::Clamp, arguments[0], true};
  if (IsName(name, "lerp") && count == 3u &&
      arguments[0] == arguments[1] && arguments[2] == ValueType::Scalar)
    return {FunctionId::Lerp, arguments[0], true};
  if (IsName(name, "inverseLerp") && count == 3u &&
      AllSame(arguments, 0u, 3u) && arguments[0] == ValueType::Scalar)
    return {FunctionId::InverseLerp, ValueType::Scalar, true};
  if (IsName(name, "remap") && count == 5u &&
      AllSame(arguments, 0u, 5u) && arguments[0] == ValueType::Scalar)
    return {FunctionId::Remap, ValueType::Scalar, true};
  if (IsName(name, "step") && count == 2u &&
      arguments[0] == ValueType::Scalar && arguments[1] != ValueType::Invalid)
    return {FunctionId::Step, arguments[1], true};
  if ((IsName(name, "smoothstep") || IsName(name, "smootherstep")) &&
      count == 3u && arguments[0] == ValueType::Scalar &&
      arguments[1] == ValueType::Scalar &&
      arguments[2] != ValueType::Invalid) {
    return {IsName(name, "smoothstep") ? FunctionId::SmoothStep :
        FunctionId::SmootherStep, arguments[2], true};
  }
  if (IsName(name, "dot") && count == 2u &&
      arguments[0] == arguments[1] && IsVector(arguments[0]))
    return {FunctionId::Dot, ValueType::Scalar, true};
  if (IsName(name, "cross") && count == 2u &&
      arguments[0] == ValueType::Vec3 && arguments[1] == ValueType::Vec3)
    return {FunctionId::Cross, ValueType::Vec3, true};
  if (IsName(name, "length") && count == 1u && IsVector(arguments[0]))
    return {FunctionId::VectorLength, ValueType::Scalar, true};
  if (IsName(name, "distance") && count == 2u &&
      arguments[0] == arguments[1] && IsVector(arguments[0]))
    return {FunctionId::Distance, ValueType::Scalar, true};
  if (IsName(name, "normalize") && count == 1u && IsVector(arguments[0]))
    return {FunctionId::Normalize, arguments[0], true};
  if ((IsName(name, "project") || IsName(name, "reject")) && count == 2u &&
      arguments[0] == arguments[1] && IsVector(arguments[0])) {
    return {IsName(name, "project") ? FunctionId::Project :
        FunctionId::Reject, arguments[0], true};
  }
  if (IsName(name, "rotateAroundAxis") && count == 3u &&
      arguments[0] == ValueType::Vec3 && arguments[1] == ValueType::Vec3 &&
      arguments[2] == ValueType::Scalar)
    return {FunctionId::RotateAroundAxis, ValueType::Vec3, true};
  if (IsName(name, "endpointMask") &&
      ((count == 1u && arguments[0] == ValueType::Scalar) ||
       (count == 3u && AllSame(arguments, 0u, 3u) &&
        arguments[0] == ValueType::Scalar)))
    return {FunctionId::EndpointMask, ValueType::Scalar, true};
  if (IsName(name, "noise1") && (count == 1u || count == 2u) &&
      arguments[0] == ValueType::Scalar &&
      (count == 1u || arguments[1] == ValueType::Scalar))
    return {FunctionId::Noise1, ValueType::Scalar, true};
  if ((IsName(name, "repeat") || IsName(name, "pingpong")) && count == 2u &&
      arguments[0] == ValueType::Scalar && arguments[1] == ValueType::Scalar) {
    return {IsName(name, "repeat") ? FunctionId::Repeat :
        FunctionId::PingPong, ValueType::Scalar, true};
  }
  if (IsName(name, "bezier2") && count == 4u &&
      arguments[0] == arguments[1] && arguments[1] == arguments[2] &&
      arguments[3] == ValueType::Scalar)
    return {FunctionId::Bezier2, arguments[0], true};
  if (IsName(name, "bezier3") && count == 5u &&
      arguments[0] == arguments[1] && arguments[1] == arguments[2] &&
      arguments[2] == arguments[3] && arguments[4] == ValueType::Scalar)
    return {FunctionId::Bezier3, arguments[0], true};
  return result;
}

struct VariableResolution {
  VariableId id = VariableId::T;
  ValueType type = ValueType::Invalid;
  bool valid = false;
};

VariableResolution ResolveVariable(std::string_view name) {
  if (IsName(name, "t")) return {VariableId::T, ValueType::Scalar, true};
  if (IsName(name, "time")) return {VariableId::Time, ValueType::Scalar, true};
  if (IsName(name, "length")) return {VariableId::Length, ValueType::Scalar, true};
  if (IsName(name, "index")) return {VariableId::Index, ValueType::Scalar, true};
  if (IsName(name, "segments")) return {VariableId::Segments, ValueType::Scalar, true};
  if (IsName(name, "seed")) return {VariableId::Seed, ValueType::Scalar, true};
  if (IsName(name, "branchIndex")) return {VariableId::BranchIndex, ValueType::Scalar, true};
  if (IsName(name, "branchDepth")) return {VariableId::BranchDepth, ValueType::Scalar, true};
  if (IsName(name, "start")) return {VariableId::Start, ValueType::Vec3, true};
  if (IsName(name, "end")) return {VariableId::End, ValueType::Vec3, true};
  if (IsName(name, "center")) return {VariableId::Center, ValueType::Vec3, true};
  if (IsName(name, "direction")) return {VariableId::Direction, ValueType::Vec3, true};
  if (IsName(name, "forward")) return {VariableId::Forward, ValueType::Vec3, true};
  if (IsName(name, "right")) return {VariableId::Right, ValueType::Vec3, true};
  if (IsName(name, "up")) return {VariableId::Up, ValueType::Vec3, true};
  return {};
}

class Parser {
public:
  explicit Parser(std::string_view expression)
  : m_expression(expression) {
  }

  ValueType parse() {
    if (m_expression.empty()) {
      fail("expression is empty");
      return ValueType::Invalid;
    }
    if (m_expression.size() > kMaximumExpressionBytes) {
      m_position = kMaximumExpressionBytes;
      fail("expression exceeds 384 bytes");
      return ValueType::Invalid;
    }
    const ValueType result = parseAddSubtract();
    skipWhitespace();
    if (!failed() && m_position != m_expression.size())
      fail("unexpected character");
    return failed() ? ValueType::Invalid : result;
  }

  bool failed() const {
    return !m_error.empty();
  }

  const std::string& error() const {
    return m_error;
  }

  size_t errorOffset() const {
    return m_errorOffset;
  }

  std::vector<std::string> takeParameters() {
    return std::move(m_parameters);
  }

  std::vector<Instruction> takeInstructions() {
    return std::move(m_instructions);
  }

private:
  ValueType parseAddSubtract() {
    ValueType left = parseMultiplyDivide();
    while (!failed()) {
      skipWhitespace();
      if (!consume('+') && !consume('-'))
        break;
      const char op = m_expression[m_position - 1u];
      const ValueType right = parseMultiplyDivide();
      if (left == ValueType::Invalid || left != right) {
        fail("addition and subtraction require matching types");
        return ValueType::Invalid;
      }
      if (!emit({op == '+' ? OpCode::Add : OpCode::Subtract, left}))
        return ValueType::Invalid;
    }
    return left;
  }

  ValueType parseMultiplyDivide() {
    ValueType left = parseUnary();
    while (!failed()) {
      skipWhitespace();
      if (!consume('*') && !consume('/'))
        break;
      const char op = m_expression[m_position - 1u];
      const ValueType right = parseUnary();
      ValueType output = ValueType::Invalid;
      if (left == right) {
        output = left;
      } else if (left == ValueType::Scalar && IsVector(right)) {
        output = right;
      } else if (IsVector(left) && right == ValueType::Scalar) {
        output = left;
      }
      if (output == ValueType::Invalid ||
          (op == '/' && left == ValueType::Scalar && IsVector(right))) {
        fail("multiplication or division uses incompatible types");
        return ValueType::Invalid;
      }
      if (!emit({op == '*' ? OpCode::Multiply : OpCode::Divide, output}))
        return ValueType::Invalid;
      left = output;
    }
    return left;
  }

  ValueType parseUnary() {
    skipWhitespace();
    if (consume('+'))
      return parseUnary();
    if (consume('-')) {
      const ValueType type = parseUnary();
      if (type == ValueType::Invalid)
        return type;
      if (!emit({OpCode::Negate, type}))
        return ValueType::Invalid;
      return type;
    }
    return parsePrimary();
  }

  ValueType parsePrimary() {
    skipWhitespace();
    if (m_position >= m_expression.size()) {
      fail("expected expression");
      return ValueType::Invalid;
    }
    const char character = m_expression[m_position];
    if ((character >= '0' && character <= '9') || character == '.')
      return parseNumber();
    if (character == '(') {
      ++m_position;
      if (!enterDepth())
        return ValueType::Invalid;
      const ValueType type = parseAddSubtract();
      leaveDepth();
      skipWhitespace();
      if (!consume(')')) {
        fail("missing closing parenthesis");
        return ValueType::Invalid;
      }
      return type;
    }
    if (!IsIdentifierStart(character)) {
      fail("expected number, variable, or function");
      return ValueType::Invalid;
    }

    const std::string_view identifier = parseIdentifier();
    skipWhitespace();
    if (consume('('))
      return parseFunction(identifier);
    if (identifier == "pi")
      return emitConstant(kPi);
    if (identifier == "tau")
      return emitConstant(kTau);

    const VariableResolution variable = ResolveVariable(identifier);
    if (variable.valid) {
      Instruction instruction;
      instruction.op = OpCode::PushVariable;
      instruction.outputType = variable.type;
      instruction.operand = static_cast<uint16_t>(variable.id);
      return emit(instruction) ? variable.type : ValueType::Invalid;
    }

    const int32_t parameter = findOrCreateParameter(identifier);
    if (parameter < 0)
      return ValueType::Invalid;
    Instruction instruction;
    instruction.op = OpCode::PushParameter;
    instruction.outputType = ValueType::Scalar;
    instruction.operand = static_cast<uint16_t>(parameter);
    return emit(instruction) ? ValueType::Scalar : ValueType::Invalid;
  }

  ValueType parseNumber() {
    const size_t start = m_position;
    bool hasDigit = false;
    while (m_position < m_expression.size() &&
           m_expression[m_position] >= '0' &&
           m_expression[m_position] <= '9') {
      hasDigit = true;
      ++m_position;
    }
    if (m_position < m_expression.size() && m_expression[m_position] == '.') {
      ++m_position;
      while (m_position < m_expression.size() &&
             m_expression[m_position] >= '0' &&
             m_expression[m_position] <= '9') {
        hasDigit = true;
        ++m_position;
      }
    }
    if (!hasDigit) {
      fail("malformed number");
      return ValueType::Invalid;
    }
    if (m_position < m_expression.size() &&
        (m_expression[m_position] == 'e' ||
         m_expression[m_position] == 'E')) {
      ++m_position;
      if (m_position < m_expression.size() &&
          (m_expression[m_position] == '+' ||
           m_expression[m_position] == '-'))
        ++m_position;
      const size_t exponentStart = m_position;
      while (m_position < m_expression.size() &&
             m_expression[m_position] >= '0' &&
             m_expression[m_position] <= '9')
        ++m_position;
      if (m_position == exponentStart) {
        fail("malformed exponent");
        return ValueType::Invalid;
      }
    }

    const std::string_view token = m_expression.substr(start, m_position - start);
    float value = 0.0f;
    const auto parsed = std::from_chars(
        token.data(), token.data() + token.size(), value,
        std::chars_format::general);
    if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size() ||
        !std::isfinite(value)) {
      fail("number is outside finite float range");
      return ValueType::Invalid;
    }
    return emitConstant(value);
  }

  ValueType parseFunction(std::string_view name) {
    if (!enterDepth())
      return ValueType::Invalid;
    std::vector<ValueType> arguments;
    skipWhitespace();
    if (!consume(')')) {
      while (!failed()) {
        if (arguments.size() >= 5u) {
          fail("function has more than five arguments");
          break;
        }
        arguments.push_back(parseAddSubtract());
        skipWhitespace();
        if (consume(')'))
          break;
        if (!consume(',')) {
          fail("expected comma or closing parenthesis");
          break;
        }
      }
    }
    leaveDepth();
    if (failed())
      return ValueType::Invalid;

    const FunctionResolution function = ResolveFunction(name, arguments);
    if (!function.valid) {
      fail("unknown function or invalid function argument types");
      return ValueType::Invalid;
    }
    Instruction instruction;
    instruction.op = OpCode::Call;
    instruction.outputType = function.output;
    instruction.operand = static_cast<uint16_t>(function.id);
    instruction.argumentCount = static_cast<uint8_t>(arguments.size());
    return emit(instruction) ? function.output : ValueType::Invalid;
  }

  std::string_view parseIdentifier() {
    const size_t start = m_position++;
    while (m_position < m_expression.size() &&
           IsIdentifierContinue(m_expression[m_position]))
      ++m_position;
    return m_expression.substr(start, m_position - start);
  }

  int32_t findOrCreateParameter(std::string_view name) {
    if (name.size() > 31u) {
      fail("parameter name exceeds 31 bytes");
      return -1;
    }
    for (size_t index = 0u; index < m_parameters.size(); ++index) {
      if (m_parameters[index] == name)
        return static_cast<int32_t>(index);
    }
    if (m_parameters.size() >= kMaximumExpressionParameters) {
      fail("expression exceeds 16 parameters");
      return -1;
    }
    m_parameters.emplace_back(name);
    return static_cast<int32_t>(m_parameters.size() - 1u);
  }

  ValueType emitConstant(float value) {
    Instruction instruction;
    instruction.op = OpCode::PushConstant;
    instruction.outputType = ValueType::Scalar;
    instruction.constant = value;
    return emit(instruction) ? ValueType::Scalar : ValueType::Invalid;
  }

  bool emit(const Instruction& instruction) {
    if (m_instructions.size() >= kMaximumExpressionInstructions) {
      fail("expression exceeds 256 instructions");
      return false;
    }
    m_instructions.push_back(instruction);
    return true;
  }

  void skipWhitespace() {
    while (m_position < m_expression.size() &&
           (m_expression[m_position] == ' ' ||
            m_expression[m_position] == '\t'))
      ++m_position;
  }

  bool consume(char expected) {
    if (m_position >= m_expression.size() ||
        m_expression[m_position] != expected)
      return false;
    ++m_position;
    return true;
  }

  bool enterDepth() {
    if (m_depth >= kMaximumParseDepth) {
      fail("expression nesting exceeds 32 levels");
      return false;
    }
    ++m_depth;
    return true;
  }

  void leaveDepth() {
    if (m_depth > 0u)
      --m_depth;
  }

  void fail(const char* message) {
    if (!m_error.empty())
      return;
    m_error = message;
    m_errorOffset = m_position;
  }

  std::string_view m_expression;
  size_t m_position = 0u;
  size_t m_depth = 0u;
  std::string m_error;
  size_t m_errorOffset = 0u;
  std::vector<std::string> m_parameters;
  std::vector<Instruction> m_instructions;
};

bool ValidateStackShape(const std::vector<Instruction>& instructions) {
  size_t depth = 0u;
  size_t maximum = 0u;
  for (const Instruction& instruction : instructions) {
    switch (instruction.op) {
      case OpCode::PushConstant:
      case OpCode::PushVariable:
      case OpCode::PushParameter:
        ++depth;
        break;
      case OpCode::Add:
      case OpCode::Subtract:
      case OpCode::Multiply:
      case OpCode::Divide:
        if (depth < 2u)
          return false;
        --depth;
        break;
      case OpCode::Negate:
        if (depth < 1u)
          return false;
        break;
      case OpCode::Call:
        if (depth < instruction.argumentCount)
          return false;
        depth = depth - instruction.argumentCount + 1u;
        break;
    }
    maximum = std::max(maximum, depth);
    if (maximum > kMaximumExpressionStack)
      return false;
  }
  return depth == 1u;
}

Value ResolveVariableValue(VariableId variable,
                           const EvaluationContext& context) {
  switch (variable) {
    case VariableId::T: return Value::scalar(context.t);
    case VariableId::Time: return Value::scalar(context.time);
    case VariableId::Length: return Value::scalar(context.length);
    case VariableId::Index: return Value::scalar(context.index);
    case VariableId::Segments: return Value::scalar(context.segments);
    case VariableId::Seed: return Value::scalar(float(context.seed));
    case VariableId::BranchIndex: return Value::scalar(context.branchIndex);
    case VariableId::BranchDepth: return Value::scalar(context.branchDepth);
    case VariableId::Start:
      return Value::vec3(context.start.x, context.start.y, context.start.z);
    case VariableId::End:
      return Value::vec3(context.end.x, context.end.y, context.end.z);
    case VariableId::Center:
      return Value::vec3(context.center.x, context.center.y, context.center.z);
    case VariableId::Direction:
      return Value::vec3(context.direction.x, context.direction.y,
                         context.direction.z);
    case VariableId::Forward:
      return Value::vec3(context.forward.x, context.forward.y,
                         context.forward.z);
    case VariableId::Right:
      return Value::vec3(context.right.x, context.right.y, context.right.z);
    case VariableId::Up:
      return Value::vec3(context.up.x, context.up.y, context.up.z);
  }
  return {};
}

bool EvaluateArithmetic(OpCode op, const Value& left, const Value& right,
                        ValueType outputType, Value& output) {
  output = MakeValue(outputType);
  const size_t count = ComponentCount(outputType);
  for (size_t index = 0u; index < count; ++index) {
    const float a = GetComponent(left, index);
    const float b = GetComponent(right, index);
    float value = 0.0f;
    switch (op) {
      case OpCode::Add: value = a + b; break;
      case OpCode::Subtract: value = a - b; break;
      case OpCode::Multiply: value = a * b; break;
      case OpCode::Divide:
        if (std::abs(b) <= kEpsilon)
          return false;
        value = a / b;
        break;
      default:
        return false;
    }
    SetComponent(output, index, value);
  }
  return IsFinite(output);
}

template<typename Operation>
bool UnaryComponentwise(const Value& input, Value& output,
                        Operation operation) {
  output = MakeValue(input.type);
  for (size_t index = 0u; index < ComponentCount(input.type); ++index) {
    float value = 0.0f;
    if (!operation(GetComponent(input, index), value))
      return false;
    SetComponent(output, index, value);
  }
  return IsFinite(output);
}

template<typename Operation>
bool BinaryComponentwise(const Value& a, const Value& b, Value& output,
                         Operation operation) {
  output = MakeValue(a.type);
  for (size_t index = 0u; index < ComponentCount(a.type); ++index) {
    float value = 0.0f;
    if (!operation(GetComponent(a, index), GetComponent(b, index), value))
      return false;
    SetComponent(output, index, value);
  }
  return IsFinite(output);
}

float Dot(const Value& a, const Value& b) {
  float result = 0.0f;
  for (size_t index = 0u; index < ComponentCount(a.type); ++index)
    result += GetComponent(a, index) * GetComponent(b, index);
  return result;
}

bool EvaluateFunction(FunctionId function,
                      const std::array<Value, 5u>& arguments,
                      uint8_t argumentCount,
                      const EvaluationContext& context,
                      Value& output) {
  const Value& a = arguments[0];
  const Value& b = arguments[1];
  const Value& c = arguments[2];
  const Value& d = arguments[3];
  const Value& e = arguments[4];
  const auto unaryStd = [&](auto operation) {
    return UnaryComponentwise(a, output, [&](float input, float& result) {
      result = operation(input);
      return std::isfinite(result);
    });
  };

  switch (function) {
    case FunctionId::Vec2:
      output = Value::vec2(a.x, b.x);
      return true;
    case FunctionId::Vec3:
      output = Value::vec3(a.x, b.x, c.x);
      return true;
    case FunctionId::X: output = Value::scalar(a.x); return true;
    case FunctionId::Y: output = Value::scalar(a.y); return true;
    case FunctionId::Z: output = Value::scalar(a.z); return true;
    case FunctionId::Sin: return unaryStd([](float v) { return std::sin(v); });
    case FunctionId::Cos: return unaryStd([](float v) { return std::cos(v); });
    case FunctionId::Tan: return unaryStd([](float v) { return std::tan(v); });
    case FunctionId::Atan: return unaryStd([](float v) { return std::atan(v); });
    case FunctionId::Exp: return unaryStd([](float v) { return std::exp(v); });
    case FunctionId::Abs: return unaryStd([](float v) { return std::abs(v); });
    case FunctionId::Floor: return unaryStd([](float v) { return std::floor(v); });
    case FunctionId::Ceil: return unaryStd([](float v) { return std::ceil(v); });
    case FunctionId::Round: return unaryStd([](float v) { return std::round(v); });
    case FunctionId::Fract:
      return unaryStd([](float v) { return v - std::floor(v); });
    case FunctionId::Saturate:
      return unaryStd([](float v) { return Clamp01(v); });
    case FunctionId::Sign:
      return unaryStd([](float v) { return v > 0.0f ? 1.0f :
          v < 0.0f ? -1.0f : 0.0f; });
    case FunctionId::Asin:
      return UnaryComponentwise(a, output, [](float v, float& result) {
        if (v < -1.0f || v > 1.0f)
          return false;
        result = std::asin(v);
        return true;
      });
    case FunctionId::Acos:
      return UnaryComponentwise(a, output, [](float v, float& result) {
        if (v < -1.0f || v > 1.0f)
          return false;
        result = std::acos(v);
        return true;
      });
    case FunctionId::Sqrt:
      return UnaryComponentwise(a, output, [](float v, float& result) {
        if (v < 0.0f)
          return false;
        result = std::sqrt(v);
        return true;
      });
    case FunctionId::Log:
      return UnaryComponentwise(a, output, [](float v, float& result) {
        if (v <= 0.0f)
          return false;
        result = std::log(v);
        return true;
      });
    case FunctionId::Atan2:
      return BinaryComponentwise(a, b, output,
          [](float y, float x, float& result) {
            result = std::atan2(y, x);
            return true;
          });
    case FunctionId::Pow:
      return BinaryComponentwise(a, b, output,
          [](float base, float exponent, float& result) {
            result = std::pow(base, exponent);
            return std::isfinite(result);
          });
    case FunctionId::Min:
      return BinaryComponentwise(a, b, output,
          [](float first, float second, float& result) {
            result = std::min(first, second);
            return true;
          });
    case FunctionId::Max:
      return BinaryComponentwise(a, b, output,
          [](float first, float second, float& result) {
            result = std::max(first, second);
            return true;
          });
    case FunctionId::Clamp:
      output = MakeValue(a.type);
      for (size_t index = 0u; index < ComponentCount(a.type); ++index) {
        const float low = GetComponent(b, index);
        const float high = GetComponent(c, index);
        if (low > high)
          return false;
        SetComponent(output, index,
                     std::clamp(GetComponent(a, index), low, high));
      }
      return true;
    case FunctionId::Lerp:
      output = MakeValue(a.type);
      for (size_t index = 0u; index < ComponentCount(a.type); ++index) {
        const float first = GetComponent(a, index);
        SetComponent(output, index,
                     first + (GetComponent(b, index) - first) * c.x);
      }
      return IsFinite(output);
    case FunctionId::InverseLerp:
      if (std::abs(b.x - a.x) <= kEpsilon)
        return false;
      output = Value::scalar((c.x - a.x) / (b.x - a.x));
      return IsFinite(output);
    case FunctionId::Remap:
      if (std::abs(b.x - a.x) <= kEpsilon)
        return false;
      output = Value::scalar(d.x + (e.x - d.x) * (c.x - a.x) / (b.x - a.x));
      return IsFinite(output);
    case FunctionId::Step:
      output = MakeValue(b.type);
      for (size_t index = 0u; index < ComponentCount(b.type); ++index)
        SetComponent(output, index, GetComponent(b, index) < a.x ? 0.0f : 1.0f);
      return true;
    case FunctionId::SmoothStep:
    case FunctionId::SmootherStep:
      if (std::abs(b.x - a.x) <= kEpsilon)
        return false;
      output = MakeValue(c.type);
      for (size_t index = 0u; index < ComponentCount(c.type); ++index) {
        const float normalized = (GetComponent(c, index) - a.x) / (b.x - a.x);
        SetComponent(output, index,
                     function == FunctionId::SmoothStep
                         ? SmoothStep01(normalized)
                         : SmootherStep01(normalized));
      }
      return true;
    case FunctionId::Dot:
      output = Value::scalar(Dot(a, b));
      return IsFinite(output);
    case FunctionId::Cross:
      output = Value::vec3(
          a.y * b.z - a.z * b.y,
          a.z * b.x - a.x * b.z,
          a.x * b.y - a.y * b.x);
      return IsFinite(output);
    case FunctionId::VectorLength:
      output = Value::scalar(std::sqrt(std::max(0.0f, Dot(a, a))));
      return IsFinite(output);
    case FunctionId::Distance: {
      float distanceSquared = 0.0f;
      for (size_t index = 0u; index < ComponentCount(a.type); ++index) {
        const float delta = GetComponent(a, index) - GetComponent(b, index);
        distanceSquared += delta * delta;
      }
      output = Value::scalar(std::sqrt(std::max(0.0f, distanceSquared)));
      return IsFinite(output);
    }
    case FunctionId::Normalize: {
      const float magnitude = std::sqrt(std::max(0.0f, Dot(a, a)));
      output = MakeValue(a.type);
      if (magnitude <= kEpsilon)
        return true;
      for (size_t index = 0u; index < ComponentCount(a.type); ++index)
        SetComponent(output, index, GetComponent(a, index) / magnitude);
      return IsFinite(output);
    }
    case FunctionId::Project:
    case FunctionId::Reject: {
      const float denominator = Dot(b, b);
      if (denominator <= kEpsilon)
        return false;
      const float scale = Dot(a, b) / denominator;
      output = MakeValue(a.type);
      for (size_t index = 0u; index < ComponentCount(a.type); ++index) {
        const float projected = GetComponent(b, index) * scale;
        SetComponent(output, index,
                     function == FunctionId::Project
                         ? projected : GetComponent(a, index) - projected);
      }
      return IsFinite(output);
    }
    case FunctionId::RotateAroundAxis: {
      const float axisLength = std::sqrt(std::max(0.0f, Dot(b, b)));
      if (axisLength <= kEpsilon)
        return false;
      const float ax = b.x / axisLength;
      const float ay = b.y / axisLength;
      const float az = b.z / axisLength;
      const float cosine = std::cos(c.x);
      const float sine = std::sin(c.x);
      const float projection = a.x * ax + a.y * ay + a.z * az;
      output = Value::vec3(
          a.x * cosine + (ay * a.z - az * a.y) * sine +
              ax * projection * (1.0f - cosine),
          a.y * cosine + (az * a.x - ax * a.z) * sine +
              ay * projection * (1.0f - cosine),
          a.z * cosine + (ax * a.y - ay * a.x) * sine +
              az * projection * (1.0f - cosine));
      return IsFinite(output);
    }
    case FunctionId::EndpointMask:
      if (argumentCount == 1u) {
        output = Value::scalar(std::sin(Clamp01(a.x) * kPi));
      } else {
        const float startMask = b.x <= 0.0f ? 1.0f :
            SmoothStep01(a.x / b.x);
        const float endMask = c.x <= 0.0f ? 1.0f :
            SmoothStep01((1.0f - a.x) / c.x);
        output = Value::scalar(std::min(startMask, endMask));
      }
      return IsFinite(output);
    case FunctionId::Noise1:
      output = Value::scalar(Noise1(
          a.x, argumentCount == 2u ? FloatSeed(b.x, context.seed) :
                                    context.seed));
      return IsFinite(output);
    case FunctionId::Repeat:
      if (b.x <= kEpsilon)
        return false;
      output = Value::scalar(a.x - std::floor(a.x / b.x) * b.x);
      return IsFinite(output);
    case FunctionId::PingPong: {
      if (b.x <= kEpsilon)
        return false;
      const float repeated = a.x - std::floor(a.x / (2.0f * b.x)) *
          (2.0f * b.x);
      output = Value::scalar(b.x - std::abs(repeated - b.x));
      return IsFinite(output);
    }
    case FunctionId::Bezier2:
    case FunctionId::Bezier3: {
      const float t = function == FunctionId::Bezier2 ? d.x : e.x;
      const float oneMinus = 1.0f - t;
      output = MakeValue(a.type);
      for (size_t index = 0u; index < ComponentCount(a.type); ++index) {
        float value = 0.0f;
        if (function == FunctionId::Bezier2) {
          value = oneMinus * oneMinus * GetComponent(a, index) +
              2.0f * oneMinus * t * GetComponent(b, index) +
              t * t * GetComponent(c, index);
        } else {
          value = oneMinus * oneMinus * oneMinus * GetComponent(a, index) +
              3.0f * oneMinus * oneMinus * t * GetComponent(b, index) +
              3.0f * oneMinus * t * t * GetComponent(c, index) +
              t * t * t * GetComponent(d, index);
        }
        SetComponent(output, index, value);
      }
      return IsFinite(output);
    }
  }
  return false;
}

} // namespace

Value Value::scalar(float value) {
  Value result;
  result.type = ValueType::Scalar;
  result.x = value;
  return result;
}

Value Value::vec2(float xValue, float yValue) {
  Value result;
  result.type = ValueType::Vec2;
  result.x = xValue;
  result.y = yValue;
  return result;
}

Value Value::vec3(float xValue, float yValue, float zValue) {
  Value result;
  result.type = ValueType::Vec3;
  result.x = xValue;
  result.y = yValue;
  result.z = zValue;
  return result;
}

ValueType Program::resultType() const {
  return m_resultType;
}

size_t Program::parameterCount() const {
  return m_parameterNames.size();
}

const std::string& Program::parameterName(size_t index) const {
  static const std::string kEmpty;
  return index < m_parameterNames.size() ? m_parameterNames[index] : kEmpty;
}

int32_t Program::findParameter(std::string_view name) const {
  for (size_t index = 0u; index < m_parameterNames.size(); ++index) {
    if (m_parameterNames[index] == name)
      return static_cast<int32_t>(index);
  }
  return -1;
}

size_t Program::instructionCount() const {
  return m_instructions.size();
}

bool Program::evaluate(
    const EvaluationContext& context,
    const std::array<float, kMaximumExpressionParameters>& parameters,
    Value& output) const {
  if (m_instructions.empty() || m_resultType == ValueType::Invalid)
    return false;

  std::array<Value, kMaximumExpressionStack> stack = {};
  size_t depth = 0u;
  auto push = [&](const Value& value) {
    if (depth >= stack.size() || !IsFinite(value))
      return false;
    stack[depth++] = value;
    return true;
  };

  for (const Instruction& instruction : m_instructions) {
    switch (instruction.op) {
      case OpCode::PushConstant:
        if (!push(Value::scalar(instruction.constant)))
          return false;
        break;
      case OpCode::PushVariable:
        if (!push(ResolveVariableValue(
                static_cast<VariableId>(instruction.operand), context)))
          return false;
        break;
      case OpCode::PushParameter:
        if (instruction.operand >= parameters.size() ||
            !push(Value::scalar(parameters[instruction.operand])))
          return false;
        break;
      case OpCode::Add:
      case OpCode::Subtract:
      case OpCode::Multiply:
      case OpCode::Divide: {
        if (depth < 2u)
          return false;
        const Value right = stack[--depth];
        const Value left = stack[--depth];
        Value result;
        if (!EvaluateArithmetic(instruction.op, left, right,
                                instruction.outputType, result) ||
            !push(result))
          return false;
        break;
      }
      case OpCode::Negate: {
        if (depth < 1u)
          return false;
        Value& value = stack[depth - 1u];
        for (size_t index = 0u; index < ComponentCount(value.type); ++index)
          SetComponent(value, index, -GetComponent(value, index));
        if (!IsFinite(value))
          return false;
        break;
      }
      case OpCode::Call: {
        if (instruction.argumentCount > 5u ||
            depth < instruction.argumentCount)
          return false;
        std::array<Value, 5u> arguments = {};
        for (size_t index = instruction.argumentCount; index > 0u; --index)
          arguments[index - 1u] = stack[--depth];
        Value result;
        if (!EvaluateFunction(static_cast<FunctionId>(instruction.operand),
                              arguments, instruction.argumentCount,
                              context, result) || !push(result))
          return false;
        break;
      }
    }
  }
  if (depth != 1u || stack[0].type != m_resultType || !IsFinite(stack[0]))
    return false;
  output = stack[0];
  return true;
}

CompileResult CompileExpression(std::string_view expression) {
  Parser parser(expression);
  const ValueType resultType = parser.parse();
  if (parser.failed())
    return {nullptr, parser.error(), parser.errorOffset()};

  std::vector<std::string> parameters = parser.takeParameters();
  std::vector<Instruction> instructions = parser.takeInstructions();
  if (resultType == ValueType::Invalid ||
      !ValidateStackShape(instructions)) {
    return {nullptr, "compiled expression has an invalid stack shape", 0u};
  }

  auto program = std::make_shared<Program>();
  program->m_resultType = resultType;
  program->m_parameterNames = std::move(parameters);
  program->m_instructions = std::move(instructions);
  return {std::move(program), {}, 0u};
}

} // namespace dxvk::war3::math
