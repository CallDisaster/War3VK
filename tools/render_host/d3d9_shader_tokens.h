#pragma once

#include <cstddef>
#include <cstdint>

namespace warvk::host::d3d9tokens {

// Explicitly restricted, programmable SM3 development profile. This is not a
// general D3D9 shader validator or a reader of arbitrary process pointers.
// The caller owns exactly count readable uint32_t values for the whole call.
// Both sides must scan their own owned copy before invoking the unbounded DXSO
// reader. Success proves the supported decoder accesses stay inside that copy;
// it does not prove Vulkan compilation, device capabilities or rendered pixels.
inline constexpr size_t MaxWords = 4096;
// Public dev-profile limit shared by proxy admission and the host. The actual
// DXSO backend may still allocate its full hardware constant layout; that does
// not broaden the constant registers admitted by this transport profile.
inline constexpr uint32_t FloatRegisters = 32;
inline constexpr uint32_t VertexVersion = 0xfffe0300u;
inline constexpr uint32_t PixelVersion = 0xffff0300u;
inline constexpr uint32_t EndToken = 0x0000ffffu;

enum class Stage : uint8_t { None, Vertex, Pixel };
enum class Error : uint8_t {
  None, NullInput, Size, Version, End, Opcode, Length,
  InstructionFlags, Operand, Register, Declaration,
};

struct Result {
  Stage stage = Stage::None;
  uint32_t bytes = 0;
  Error error = Error::None;
  bool accepted() const noexcept {
    return error == Error::None && stage != Stage::None;
  }
};

inline const char* ErrorName(Error error) noexcept {
  switch (error) {
    case Error::None: return "None";
    case Error::NullInput: return "NullInput";
    case Error::Size: return "Size";
    case Error::Version: return "Version";
    case Error::End: return "End";
    case Error::Opcode: return "Opcode";
    case Error::Length: return "Length";
    case Error::InstructionFlags: return "InstructionFlags";
    case Error::Operand: return "Operand";
    case Error::Register: return "Register";
    case Error::Declaration: return "Declaration";
  }
  return "Invalid";
}

namespace detail {

inline Result Refuse(Error error) noexcept {
  Result result;
  result.error = error;
  return result;
}

inline uint32_t RegisterType(uint32_t token) noexcept {
  return ((token >> 28) & 7u) | ((token >> 8) & 24u);
}
inline uint32_t RegisterNumber(uint32_t token) noexcept {
  return token & 0x7ffu;
}

// No relative operands, predicate/coissue, destination shift, flow control,
// integer/bool constants, miscellaneous registers, vertex textures or MRTs.
inline bool DestinationBits(uint32_t token, bool centroid = false) noexcept {
  const uint32_t allowed = 0xf03f1fffu | (centroid ? 0x00400000u : 0u);
  return (token & 0x80000000u) != 0 && (token & ~allowed) == 0
      && (token & 0x000f0000u) != 0;
}
inline bool SourceBits(uint32_t token) noexcept {
  constexpr uint32_t allowed = 0xffff1fffu;
  const uint32_t modifier = (token >> 24) & 15u;
  return (token & 0x80000000u) != 0 && (token & ~allowed) == 0
      && (modifier == 0 || modifier == 1 || modifier == 11 || modifier == 12);
}

struct Registers {
  uint32_t inputs = 0;
  uint32_t outputs = 0;
  uint32_t inputSemantics = 0;
  uint32_t outputSemantics = 0;
  uint32_t positionRegister = 12;
  uint32_t positionWritten = 0;
  uint32_t colorWritten = 0;
  uint32_t samplers = 0;
  bool executable = false;
  bool definitions[FloatRegisters] = {};
};

inline Error Declare(Stage stage, uint32_t semantic, uint32_t token,
                     Registers& regs) noexcept {
  const bool pixel = stage == Stage::Pixel;
  if (!DestinationBits(token, pixel) || (token & 0x00300000u))
    return Error::Operand;
  const uint32_t type = RegisterType(token);
  const uint32_t number = RegisterNumber(token);
  if (type == 10) {
    // The finite pixel profile supports 2D s0..s7, not vertex/cube/volume samples.
    if (!pixel || number >= 8) return Error::Register;
    if (semantic != 0x90000000u || (token & 0x00400000u)
        || (regs.samplers & (1u << number))) return Error::Declaration;
    regs.samplers |= 1u << number;
    return Error::None;
  }
  const bool input = type == 1;
  if (!input && !(type == 6 && !pixel)) return Error::Register;
  if (number >= (input ? (pixel ? 10u : 16u) : 12u)) return Error::Register;
  if ((semantic & 0x80000000u) == 0 || (semantic & ~0x800f000fu))
    return Error::Declaration;
  const uint32_t usage = semantic & 15u;
  const uint32_t index = (semantic >> 16) & 15u;
  uint32_t key = 0;
  if (usage == 0 && index == 0 && !pixel) key = 0; // POSITION0
  else if (usage == 5 && index < 8) key = 1 + index; // TEXCOORD0..7
  else if (usage == 10 && index < 2) key = 9 + index; // COLOR0..1
  else return Error::Declaration;
  auto& declared = input ? regs.inputs : regs.outputs;
  auto& semantics = input ? regs.inputSemantics : regs.outputSemantics;
  if ((declared & (1u << number)) || (semantics & (1u << key)))
    return Error::Declaration;
  declared |= 1u << number;
  semantics |= 1u << key;
  if (!input && usage == 0) regs.positionRegister = number;
  return Error::None;
}

inline Error Destination(Stage stage, uint32_t token, Registers& regs) noexcept {
  if (!DestinationBits(token)) return Error::Operand;
  const uint32_t type = RegisterType(token);
  const uint32_t number = RegisterNumber(token);
  if (type == 0 && number < 32) return Error::None;
  if (stage == Stage::Vertex && type == 6 && number < 12) {
    if (!(regs.outputs & (1u << number))) return Error::Declaration;
    if (number == regs.positionRegister) regs.positionWritten |= (token >> 16) & 15u;
    return Error::None;
  }
  if (stage == Stage::Pixel && type == 8 && number == 0) {
    regs.colorWritten |= (token >> 16) & 15u;
    return Error::None;
  }
  return Error::Register;
}

inline Error Source(Stage stage, uint32_t token, const Registers& regs) noexcept {
  if (!SourceBits(token)) return Error::Operand;
  const uint32_t type = RegisterType(token);
  const uint32_t number = RegisterNumber(token);
  if (type == 0 && number < 32) return Error::None;
  if (type == 2 && number < FloatRegisters) return Error::None;
  if (type == 1 && number < (stage == Stage::Vertex ? 16u : 10u))
    return (regs.inputs & (1u << number)) ? Error::None : Error::Declaration;
  return Error::Register;
}

} // namespace detail

inline Result Scan(const uint32_t* words, size_t count) noexcept {
  if (!words) return detail::Refuse(Error::NullInput);
  if (count < 2 || count > MaxWords) return detail::Refuse(Error::Size);
  const Stage stage = words[0] == VertexVersion ? Stage::Vertex
      : words[0] == PixelVersion ? Stage::Pixel : Stage::None;
  if (stage == Stage::None) return detail::Refuse(Error::Version);
  detail::Registers regs;
  size_t cursor = 1;
  while (cursor < count) {
    const uint32_t token = words[cursor++];
    const uint32_t opcode = token & 0xffffu;
    if (opcode == 0xffffu) {
      if (token != EndToken || cursor != count) return detail::Refuse(Error::End);
      if (!regs.executable || (stage == Stage::Vertex ? regs.positionWritten : regs.colorWritten) != 15)
        return detail::Refuse(Error::Declaration);
      return Result{stage, uint32_t(count * sizeof(uint32_t)), Error::None};
    }
    if (opcode == 0xfffeu) {
      if (token & 0x80000000u) return detail::Refuse(Error::InstructionFlags);
      const size_t length = (token >> 16) & 0x7fffu;
      if (length > count - cursor) return detail::Refuse(Error::Length);
      cursor += length; // COMMENT contents are never parsed as instructions.
      continue;
    }
    // Only the 4-bit SM3 parameter count and 16-bit opcode may be set.
    // In particular, TEX is regular TEXLD, not project/bias variants.
    if (token & ~0x0f00ffffu) return detail::Refuse(Error::InstructionFlags);
    uint32_t expected = 0;
    switch (opcode) {
      case 0: expected = 0; break; // NOP
      case 1: expected = 2; break; // MOV
      case 2: case 3: case 5: case 8: case 9: expected = 3; break; // ADD/SUB/MUL/DP3/DP4
      case 4: expected = 4; break; // MAD
      case 31: expected = 2; break; // DCL
      case 66: expected = 3; break; // TEX (PS3)
      case 81: expected = 5; break; // DEF (float4)
      default: return detail::Refuse(Error::Opcode);
    }
    const uint32_t length = (token >> 24) & 15u;
    if (length != expected || length > count - cursor) return detail::Refuse(Error::Length);
    const uint32_t* args = words + cursor;
    cursor += length;
    if (opcode == 0) continue;
    if (opcode == 31) {
      if (regs.executable) return detail::Refuse(Error::Declaration);
      const auto error = detail::Declare(stage, args[0], args[1], regs);
      if (error != Error::None) return detail::Refuse(error);
      continue;
    }
    if (opcode == 81) {
      if (regs.executable || !detail::DestinationBits(args[0])
          || (args[0] & 0x00300000u) || ((args[0] >> 16) & 15u) != 15)
        return detail::Refuse(Error::Operand);
      const uint32_t number = detail::RegisterNumber(args[0]);
      if (detail::RegisterType(args[0]) != 2 || number >= FloatRegisters)
        return detail::Refuse(Error::Register);
      if (regs.definitions[number]) return detail::Refuse(Error::Declaration);
      regs.definitions[number] = true;
      continue; // The four immediate words are data, not register tokens.
    }
    regs.executable = true;
    const auto dstError = detail::Destination(stage, args[0], regs);
    if (dstError != Error::None) return detail::Refuse(dstError);
    if (opcode == 66) {
      if (stage != Stage::Pixel || detail::RegisterType(args[0]) != 0)
        return detail::Refuse(Error::Register);
      const auto error = detail::Source(stage, args[1], regs);
      if (error != Error::None) return detail::Refuse(error);
      // Exact unmodified s0..s7, with an earlier matching 2D declaration.
      if ((args[2] & ~7u) != 0xa0e40800u) return detail::Refuse(Error::Operand);
      if (!(regs.samplers & (1u << (args[2] & 7u)))) return detail::Refuse(Error::Declaration);
    } else {
      for (uint32_t i = 1; i < length; ++i) {
        const auto error = detail::Source(stage, args[i], regs);
        if (error != Error::None) return detail::Refuse(error);
      }
    }
  }
  return detail::Refuse(Error::End);
}

} // namespace warvk::host::d3d9tokens
