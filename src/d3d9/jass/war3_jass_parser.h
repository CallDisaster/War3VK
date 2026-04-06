#pragma once
#include "war3_types.h"

// JASS 字节码与类型定义（精简版，仅保留核心枚举）
class Parser {
public:
  // 字节码
  enum OPCODE : byte_t {
    OPCODE_MINLIMIT   = 0x00,
    OPCODE_ENDPROGRAM = 0x01,
    OPCODE_OLDJUMP    = 0x02,
    OPCODE_FUNCTION   = 0x03,
    OPCODE_ENDFUNCTION= 0x04,
    OPCODE_LOCAL      = 0x05,
    OPCODE_GLOBAL     = 0x06,
    OPCODE_CONSTANT   = 0x07,
    OPCODE_FUNCARG    = 0x08,
    OPCODE_EXTENDS    = 0x09,
    OPCODE_TYPE       = 0x0A,
    OPCODE_POPN       = 0x0B,
    OPCODE_MOVRL      = 0x0C,
    OPCODE_MOVRR      = 0x0D,
    OPCODE_MOVRV      = 0x0E,
    OPCODE_MOVRC      = 0x0F,
    OPCODE_MOVRA      = 0x10,
    OPCODE_MOVVR      = 0x11,
    OPCODE_MOVAR      = 0x12,
    OPCODE_PUSH       = 0x13,
    OPCODE_POP        = 0x14,
    OPCODE_CALLNATIVE = 0x15,
    OPCODE_CALLJASS   = 0x16,
    OPCODE_I2R        = 0x17,
    OPCODE_AND        = 0x18,
    OPCODE_OR         = 0x19,
    OPCODE_EQUAL      = 0x1A,
    OPCODE_NOTEQUAL   = 0x1B,
    OPCODE_LESSEREQUAL= 0x1C,
    OPCODE_GREATEREQUAL=0x1D,
    OPCODE_LESSER     = 0x1E,
    OPCODE_GREATER    = 0x1F,
    OPCODE_ADD        = 0x20,
    OPCODE_SUB        = 0x21,
    OPCODE_MUL        = 0x22,
    OPCODE_DIV        = 0x23,
    OPCODE_MOD        = 0x24,
    OPCODE_NEGATE     = 0x25,
    OPCODE_NOT        = 0x26,
    OPCODE_RETURN     = 0x27,
    OPCODE_LABEL      = 0x28,
    OPCODE_JUMPIFTRUE = 0x29,
    OPCODE_JUMPIFFALSE= 0x2A,
    OPCODE_JUMP       = 0x2B,
    OPCODE_MAXLIMIT   = 0x2C,

    OPCODE_NULL,
  };

  // 变量类型
  enum VTYPE : uint32_t {
    VT_NOTHING        = 0x0,
    VT_UNKNOWN        = 0x1,
    VT_NULL           = 0x2,
    VT_CODE           = 0x3,
    VT_INTEGER        = 0x4,
    VT_REAL           = 0x5,
    VT_STRING         = 0x6,
    VT_HANDLE         = 0x7,
    VT_BOOLEAN        = 0x8,
    VT_INTEGER_ARRAY  = 0x9,
    VT_REAL_ARRAY     = 0xA,
    VT_STRING_ARRAY   = 0xB,
    VT_HANDLE_ARRAY   = 0xC,
    VT_BOOLEAN_ARRAY  = 0xD,
  };
};
