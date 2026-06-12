#include "war3_shadow_filter_policy.h"

#include "../../d3d9_war3_debug.h"
#include "../core/war3_internal_test_config.h"
#include "../core/war3_memory.h"
#include "../game/war3_agent.h"
#include "../game/war3_unit.h"

#include <array>
#include <atomic>
#include <cstring>

namespace dxvk::war3::hooks::shadowfilter {

namespace {

/**
 * @brief 仅按 ASCII 规则进行大小写不敏感比较，避免本地化副作用。
 */
bool EqualsIgnoreCaseAscii(const char* lhs, const char* rhs) {
  if (!lhs || !rhs)
    return false;

  while (*lhs && *rhs) {
    unsigned char a = static_cast<unsigned char>(*lhs);
    unsigned char b = static_cast<unsigned char>(*rhs);
    if (a >= 'A' && a <= 'Z')
      a = static_cast<unsigned char>(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z')
      b = static_cast<unsigned char>(b - 'A' + 'a');
    if (a != b)
      return false;
    ++lhs;
    ++rhs;
  }

  return (*lhs == '\0') && (*rhs == '\0');
}

/**
 * @brief ASCII 不区分大小写子串匹配。
 */
bool ContainsIgnoreCaseAscii(const char* text, const char* token) {
  if (!text || !token || !token[0])
    return false;

  const size_t textLen = std::strlen(text);
  const size_t tokenLen = std::strlen(token);
  if (tokenLen == 0u || textLen < tokenLen)
    return false;

  for (size_t i = 0; i + tokenLen <= textLen; ++i) {
    bool matched = true;
    for (size_t j = 0; j < tokenLen; ++j) {
      unsigned char a = static_cast<unsigned char>(text[i + j]);
      unsigned char b = static_cast<unsigned char>(token[j]);
      if (a >= 'A' && a <= 'Z')
        a = static_cast<unsigned char>(a - 'A' + 'a');
      if (b >= 'A' && b <= 'Z')
        b = static_cast<unsigned char>(b - 'A' + 'a');
      if (a != b) {
        matched = false;
        break;
      }
    }
    if (matched)
      return true;
  }
  return false;
}

bool IsWhitelistedRegisterSource(ShadowRegisterSource source) {
  return source == ShadowRegisterSource::SelectionCircleColorFriend ||
         source == ShadowRegisterSource::MarkColorOcclusion;
}

bool IsBuildingLikeOwner(ShadowOwnerKind kind) {
  return kind == ShadowOwnerKind::Building ||
         kind == ShadowOwnerKind::Destructible;
}

bool IsUnitLikeOwner(ShadowOwnerKind kind) {
  return kind == ShadowOwnerKind::Unit || kind == ShadowOwnerKind::Item;
}

bool IsLikelyBuildingStyleShadowKey(const char* key) {
  if (!key || !key[0])
    return false;

  if (IsBlockedShadowKey(key))
    return true;

  const size_t len = std::strlen(key);
  if (len != 4u)
    return false;

  unsigned char c0 = static_cast<unsigned char>(key[0]);
  if (c0 >= 'A' && c0 <= 'Z')
    c0 = static_cast<unsigned char>(c0 - 'A' + 'a');
  return c0 == static_cast<unsigned char>('o');
}

/**
 * @brief 识别 UberSplat 纹理 key（常见于建筑/矿点等静态地面贴花阴影）。
 */
bool IsLikelyUberSplatShadowKey(const char* key) {
  if (!key || !key[0])
    return false;
  return ContainsIgnoreCaseAscii(key, "ubersplat");
}

/**
 * @brief 识别原生阴影贴图 key（非 splat 融合贴花）。
 */
bool IsLikelyNativeShadowTextureKey(const char* key) {
  if (!key || !key[0])
    return false;

  if (ContainsIgnoreCaseAscii(key, "replaceabletextures\\shadows\\"))
    return true;

  if (EqualsIgnoreCaseAscii(key, "Shadow") ||
      EqualsIgnoreCaseAscii(key, "ShadowFlyer")) {
    return true;
  }

  if (ContainsIgnoreCaseAscii(key, "buildingshadow"))
    return true;

  return false;
}

/**
 * @brief 识别 Selection 贴图 key（应保留，不属于阴影本体）。
 */
bool IsLikelySelectionTextureKey(const char* key) {
  if (!key || !key[0])
    return false;
  return ContainsIgnoreCaseAscii(key, "replaceabletextures\\selection\\");
}

uint32_t ByteSwapU32(uint32_t v) {
  return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
         ((v & 0x00FF0000u) >> 8) | ((v & 0xFF000000u) >> 24);
}

uint32_t NormalizeFourCcEditorSecondChar(uint32_t v) {
  const uint32_t c1 = (v >> 16) & 0xFFu;
  if (c1 >= static_cast<uint32_t>('a') &&
      c1 <= static_cast<uint32_t>('z')) {
    v = (v & 0xFF00FFFFu) | ((c1 - 0x20u) << 16);
  }
  return v;
}

bool MatchesBlockedFourCC(uint32_t fourcc) {
  for (uint32_t i = 0;
       i < dxvk::war3::internal::kNativeShadowBlockedFourCCsCount; ++i) {
    if (fourcc == dxvk::war3::internal::kNativeShadowBlockedFourCCs[i])
      return true;
  }
  return false;
}

}  // namespace

const char* ToString(ShadowRegisterSource source) {
  switch (source) {
  case ShadowRegisterSource::StaticStamp:
    return "StaticStamp";
  case ShadowRegisterSource::EmitterStamp:
    return "EmitterStamp";
  case ShadowRegisterSource::SelectionCircleColorFriend:
    return "SelectionCircleColorFriend";
  case ShadowRegisterSource::MarkColorOcclusion:
    return "MarkColorOcclusion";
  case ShadowRegisterSource::WithParams:
    return "WithParams";
  case ShadowRegisterSource::ObjectBridge:
    return "ObjectBridge";
  case ShadowRegisterSource::FromPoint:
    return "FromPoint";
  case ShadowRegisterSource::FromTwoPoints:
    return "FromTwoPoints";
  default:
    return "Unknown";
  }
}

const char* ToString(ShadowOwnerKind kind) {
  switch (kind) {
  case ShadowOwnerKind::Unit:
    return "Unit";
  case ShadowOwnerKind::Building:
    return "Building";
  case ShadowOwnerKind::Destructible:
    return "Destructible";
  case ShadowOwnerKind::Item:
    return "Item";
  default:
    return "Unknown";
  }
}

ShadowRegisterDecision DecideRegisterImage(const ShadowRegisterContext& ctx) {
  ShadowRegisterDecision decision = {};
  decision.blocked = false;
  decision.reason = "PassThrough";

  if (ctx.mode < 1u)
    return decision;

  if (ctx.mode == 1u &&
      dxvk::war3::internal::kNativeShadowRegisterBlockAllWhenMode1) {
    decision.blocked = true;
    decision.reason = "Mode1_BlockAllRegisterImage";
    return decision;
  }

  // StrictMode=off 时保留旧配置语义，避免历史调试开关失效。
  if (!dxvk::war3::internal::kNativeShadowRegisterPolicyStrictMode1) {
    if (ctx.source == ShadowRegisterSource::StaticStamp &&
        dxvk::war3::internal::kNativeShadowBlockStaticStampRegisterWhenMode1) {
      decision.blocked = true;
      decision.reason = "Legacy_BlockStaticStamp";
      return decision;
    }
    if (ctx.source == ShadowRegisterSource::EmitterStamp &&
        dxvk::war3::internal::kNativeShadowBlockEmitterStampRegisterWhenMode1) {
      decision.blocked = true;
      decision.reason = "Legacy_BlockEmitterStamp";
      return decision;
    }
    return decision;
  }

  if (IsWhitelistedRegisterSource(ctx.source)) {
    decision.reason = "Mode1_WhitelistSource";
    return decision;
  }

  if (IsLikelySelectionTextureKey(ctx.hasKey ? ctx.key : nullptr)) {
    decision.reason = "Mode1_AllowSelectionTextureKey";
    return decision;
  }

  if (ctx.source == ShadowRegisterSource::StaticStamp &&
      dxvk::war3::internal::kNativeShadowBlockStaticStampRegisterWhenMode1) {
    decision.blocked = true;
    decision.reason = "Mode1_BlockStaticStamp";
    return decision;
  }

  if (dxvk::war3::internal::kNativeShadowRegisterBlockShadowTextureKeyWhenMode1 &&
      IsLikelyNativeShadowTextureKey(ctx.hasKey ? ctx.key : nullptr)) {
    decision.blocked = true;
    decision.reason = "Mode1_BlockShadowTextureKey";
    return decision;
  }

  // WithParams 是投影器上游常见入口；当 key 命中 UberSplat 时直接阻断，
  // 用于规避 owner 指针不可解导致的“建筑阴影误放行”。
  if (dxvk::war3::internal::kNativeShadowRegisterBlockWithParamsUberSplatWhenMode1 &&
      ctx.source == ShadowRegisterSource::WithParams &&
      IsLikelyUberSplatShadowKey(ctx.hasKey ? ctx.key : nullptr)) {
    decision.blocked = true;
    decision.reason = "Mode1_BlockWithParamsUberSplat";
    return decision;
  }

  if (!dxvk::war3::internal::kNativeShadowRegisterOwnerKindFilterEnabled) {
    if (ctx.source == ShadowRegisterSource::EmitterStamp &&
        dxvk::war3::internal::kNativeShadowBlockEmitterStampRegisterWhenMode1) {
      decision.blocked = true;
      decision.reason = "Mode1_BlockEmitter_NoOwnerFilter";
    }
    return decision;
  }

  if (IsBuildingLikeOwner(ctx.ownerKind)) {
    decision.blocked = true;
    decision.reason = "Mode1_BlockBuildingOwner";
    return decision;
  }

  if (IsUnitLikeOwner(ctx.ownerKind)) {
    decision.reason = "Mode1_AllowUnitOrItemOwner";
    return decision;
  }

  // Unknown owner：默认放行，仅在“建筑样式 key + type={0,4}”时阻断。
  if (dxvk::war3::internal::kNativeShadowRegisterUnknownOwnerTypeKeyBlockEnabled &&
      (ctx.argType == 0 || ctx.argType == 4) &&
      IsLikelyBuildingStyleShadowKey(ctx.hasKey ? ctx.key : nullptr)) {
    decision.blocked = true;
    decision.reason = "Mode1_BlockUnknownOwnerByTypeAndKey";
    return decision;
  }

  decision.reason = "Mode1_AllowUnknownOwner";
  return decision;
}

bool ReadAsciiCStringSafe(const char* src, char* dst, size_t dstSize) {
  if (!src || !dst || dstSize < 2)
    return false;

  dst[0] = '\0';
  for (size_t i = 0; i + 1 < dstSize; ++i) {
    const char* p = src + i;
    if (!dxvk::war3::IsReadableRange(reinterpret_cast<const void*>(p), 1))
      return false;

    const unsigned char ch = static_cast<unsigned char>(*p);
    if (ch == 0) {
      dst[i] = '\0';
      return i > 0;
    }

    // 只接受可打印 ASCII，避免把二进制块误判成 key。
    if (ch < 0x20 || ch > 0x7E)
      return false;

    dst[i] = static_cast<char>(ch);
  }

  dst[dstSize - 1] = '\0';
  return true;
}

bool IsBlockedShadowKey(const char* key) {
  if (!key || !key[0])
    return false;

  for (uint32_t i = 0;
       i < dxvk::war3::internal::kNativeShadowBlockedUbersplatKeyCount; ++i) {
    const char* blocked =
        dxvk::war3::internal::kNativeShadowBlockedUbersplatKeys[i];
    if (blocked && blocked[0] && EqualsIgnoreCaseAscii(key, blocked))
      return true;
  }
  return false;
}

bool IsBlockedFourCC(uint32_t fourcc) {
  if (fourcc == 0)
    return false;

  // 与 D3D9 mesh gate 保持一致：黑名单以编辑器显示顺序保存，但运行时
  // 可能读到编辑器序或内存序；两种都尝试，并兼容 YTlc/Ytlc 第二字符大小写。
  const uint32_t direct = fourcc;
  const uint32_t swapped = ByteSwapU32(fourcc);
  return MatchesBlockedFourCC(direct) || MatchesBlockedFourCC(swapped) ||
         MatchesBlockedFourCC(NormalizeFourCcEditorSecondChar(direct)) ||
         MatchesBlockedFourCC(NormalizeFourCcEditorSecondChar(swapped));
}

bool TryExtractShadowObjectFourCC(void* obj, uint32_t& outFourCC) {
  outFourCC = 0;
  if (!obj)
    return false;

  uint32_t fourcc = 0;
  if (dxvk::war3::SafeReadU32Fast(obj, 0x30, fourcc) && fourcc != 0u) {
    outFourCC = fourcc;
    return true;
  }

  dxvk::war3::game::UnitWrapper asUnit(obj);
  if (asUnit.IsValid()) {
    fourcc = asUnit.GetRawcode();
    if (fourcc != 0u) {
      outFourCC = fourcc;
      return true;
    }
  }

  dxvk::war3::game::AgentWrapper asAgent(obj);
  if (asAgent.IsValid()) {
    void* unitPtr = asAgent.GetUnitPtr();
    if (unitPtr) {
      dxvk::war3::game::UnitWrapper unit(unitPtr);
      if (unit.IsValid()) {
        fourcc = unit.GetRawcode();
        if (fourcc != 0u) {
          outFourCC = fourcc;
          return true;
        }
      }
    }
  }

  return false;
}

void RecordProjectorKeySample(const char* key) {
  if (!key || !key[0])
    return;

  static std::array<std::array<char, 64>, 24> s_seenKeys = {};
  static std::atomic<uint32_t> s_seenCount{0};

  const uint32_t seen = s_seenCount.load(std::memory_order_relaxed);
  const uint32_t cap = static_cast<uint32_t>(s_seenKeys.size());
  const uint32_t lim = (seen < cap) ? seen : cap;
  for (uint32_t i = 0; i < lim; ++i) {
    if (EqualsIgnoreCaseAscii(s_seenKeys[i].data(), key))
      return;
  }

  if (seen >= cap)
    return;

  uint32_t idx = seen;
  if (!s_seenCount.compare_exchange_strong(idx, seen + 1u,
                                           std::memory_order_relaxed)) {
    return;
  }

  std::array<char, 64>& slot = s_seenKeys[seen];
  std::memset(slot.data(), 0, slot.size());
  const size_t len = std::strlen(key);
  const size_t copyLen = (len < (slot.size() - 1u)) ? len : (slot.size() - 1u);
  std::memcpy(slot.data(), key, copyLen);
  slot[copyLen] = '\0';

  war3dbg::Print(
      "DXVK War3Hook: Projector key sample[%u/%u] = '%s'\n",
      static_cast<unsigned>(seen + 1u), static_cast<unsigned>(cap), slot.data());
}

}  // namespace dxvk::war3::hooks::shadowfilter
