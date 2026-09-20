#pragma once

#include "war3_native_light_policy.h"
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>

namespace dxvk::war3::native_light {

// MDX identities, parsed-record ordinals and native output slots are distinct.
// No native pointer or data buffer is retained in this owned description.
struct Source {
  uint32_t policyIndex = UINT32_MAX;
  uint32_t objectId = 0;
  uint32_t ordinal = 0;
  uint32_t count = 0;
  float range = 0;
  uint32_t catalog = UINT32_MAX;
};

// Resource names, not OS filenames. Case/slash aliases have one rule. Never
// accept traversal, drive letters, wire delimiters, controls or truncated keys.
inline bool NormalizeModelLightPath(std::string_view input, std::string& out) {
  out.clear();
  if (input.empty() || input.size() > 240) return false;
  for (unsigned char c : input) {
    if (c < 32 || c >= 127 || c == ':' || c == ';' || c == '"') return false;
    out += render::NativeLightPathChar(char(c));
  }
  if (out.front() == '\\' || out.back() == '\\') return false;
  size_t start = 0;
  while (start < out.size()) {
    const auto end = out.find('\\', start);
    const auto part = std::string_view(out).substr(start,
      end == std::string::npos ? out.size()-start : end-start);
    if (part.empty() || part == "." || part == "..") return false;
    if (end == std::string::npos) break;
    start = end+1;
  }
  // Warcraft's model APIs also accept .mdl names for binary MDX resources.
  if (out.size() < 5) return false;
  if (out.compare(out.size()-4, 4, ".mdl") == 0) out.back() = 'x';
  return out.compare(out.size()-4, 4, ".mdx") == 0;
}

struct ModelLightSources {
  std::array<Source, 16> lights = {};
  uint32_t count = 0;
};

// Cold-load description only. The native evaluator remains authoritative for
// animation, visibility and world transforms. Unsupported attenuation tracks
// are not guessed from their first key. No file I/O or native pointer retained.
inline bool DecodeModelLightSources(const uint8_t* data, size_t size,
    ModelLightSources& out) noexcept {
  out = {};
  if (!data || size < 12 || size > 4u*1024u*1024u || std::memcmp(data,"MDLX",4)) return false;
  auto u32 = [data](size_t p) { uint32_t v; std::memcpy(&v,data+p,4); return v; };
  auto f32 = [data](size_t p) { float v; std::memcpy(&v,data+p,4); return v; };
  bool version = false, lite = false;
  uint32_t total = 0;
  std::array<uint32_t,16> ids = {};
  for (size_t p=4; p<size;) {
    if (size-p < 8) return false;
    const auto tag=u32(p), bytes=u32(p+4); p+=8;
    if (bytes > size-p) return false;
    const size_t end=p+bytes;
    if (tag == 0x53524556u) {
      if (version || bytes != 4 || u32(p) != 800) return false;
      version=true;
    }
    if (tag == 0x4554494cu) {
      if (lite) return false;
      lite=true;
      while (p<end) {
        if (end-p<144 || total>=16) return false;
        const auto record=u32(p), node=u32(p+4);
        if (record<144 || record>end-p || node<96 || node>record-48) return false;
        const auto id=u32(p+88);
        if (id==UINT32_MAX) return false;
        for (uint32_t i=0;i<total;++i) if (ids[i]==id) return false;
        ids[total]=id;
        const size_t f=p+4+node;
        const float begin=f32(f+4), range=f32(f+8);
        bool eligible=u32(f)==0 && std::isfinite(begin) && std::isfinite(range) &&
          begin>=0 && range>begin && range<=10000;
        for (size_t t=f+44;t<p+record;) {
          if (p+record-t<16) return false;
          const auto track=u32(t), n=u32(t+4), interpolation=u32(t+8);
          uint32_t components=0;
          if (track==0x43414c4bu || track==0x43424c4bu) components=3;
          if (track==0x49414c4bu || track==0x49424c4bu || track==0x56414c4bu) components=1;
          if (track==0x53414c4bu || track==0x45414c4bu) { components=1; eligible=false; }
          if (!components || interpolation>3) return false;
          const uint64_t length=16ull+n*(4ull+4ull*components*(interpolation>1 ? 3 : 1));
          if (length>p+record-t) return false;
          t+=size_t(length);
        }
        if (eligible) out.lights[out.count++]={UINT32_MAX,id,total,0,range};
        ++total; p+=record;
      }
    }
    p=end;
  }
  for (uint32_t i=0;i<out.count;++i) out.lights[i].count=total;
  return version && lite && out.count;
}

inline bool DecodeReviewedSource(const uint8_t* data, size_t size,
    std::string_view path, std::string_view sha256, Source& out) noexcept {
  out = {};
  const auto& policy = render::kNativeModelLightShadowCandidates;
  uint32_t policyIndex = UINT32_MAX;
  for (uint32_t i = 0; i < policy.size(); ++i)
    if (size == policy[i].bytes && sha256 == policy[i].sha256 &&
        render::NativeLightSamePath(path, policy[i].path))
      policyIndex = i;
  if (policyIndex == UINT32_MAX || !data || size < 12 ||
      std::memcmp(data, "MDLX", 4)) return false;
  auto u32 = [data](size_t p) { uint32_t v; std::memcpy(&v, data+p, 4); return v; };
  auto f32 = [data](size_t p) { float v; std::memcpy(&v, data+p, 4); return v; };
  bool version = false, found = false, hasLite = false;
  size_t pos = 4;
  while (pos < size) {
    if (size-pos < 8) return false;
    const uint32_t tag = u32(pos), bytes = u32(pos+4);
    pos += 8;
    if (bytes > size-pos) return false;
    const size_t end = pos+bytes;
    if (tag == 0x53524556u) { // VERS
      if (version || bytes != 4 || u32(pos) != 800) return false;
      version = true;
    }
    if (tag == 0x4554494cu) { // LITE
      if (hasLite) return false;
      hasLite = true;
      uint32_t ordinal = 0;
      while (pos < end) {
        if (end-pos < 144 || ordinal >= 16) return false;
        const uint32_t record = u32(pos), node = u32(pos+4);
        if (record < 144 || record > end-pos || node < 96 ||
            node > record-48) return false;
        const size_t fields = pos+4+node;
        const uint32_t id = u32(pos+88);
        if (id == policy[policyIndex].lightObjectId) {
          if (found || u32(fields) != 0 || u32(pos+92) != UINT32_MAX)
            return false; // reviewed root Omni only
          const float start = f32(fields+4), range = f32(fields+8);
          if (!std::isfinite(start) || !std::isfinite(range) || start < 0 ||
              range <= start || range > 10000) return false;
          // Reviewed resources may animate RGB/intensity/visibility in native
          // evaluation. Animated attenuation is not silently treated as static.
          size_t track = fields+44;
          while (track < pos+record) {
            if (pos+record-track < 16) return false;
            const uint32_t t = u32(track), n = u32(track+4), interp = u32(track+8);
            if (t == 0x53414c4bu || t == 0x45414c4bu) return false; // KLAS/KLAE
            uint32_t components = 0;
            if (t == 0x43414c4bu || t == 0x43424c4bu) components = 3; // KLAC/KLBC
            if (t == 0x49414c4bu || t == 0x49424c4bu || t == 0x56414c4bu)
              components = 1; // KLAI/KLBI/KLAV
            if (!components || interp > 3) return false;
            const uint64_t length = 16ull+n*(4ull+4ull*components*(interp>1 ? 3 : 1));
            if (length > pos+record-track) return false;
            track += size_t(length);
          }
          out = {policyIndex, id, ordinal, 0, range};
          found = true;
        }
        ++ordinal;
        pos += record;
      }
      out.count = ordinal;
    }
    pos = end;
  }
  return version && found && hasLite && out.count != 0;
}

struct NativeValue {
  uint32_t enabled = 0, positional = 0;
  float position[3] = {};
  uint32_t ambientColor = 0, directColor = 0;
  float ambientIntensity = 0, directIntensity = 0;
  uint32_t references = 0;
  float selectionScore = 0; // NOT range
};
static_assert(sizeof(NativeValue) == 44);

inline bool Usable(const NativeValue& value) noexcept {
  return value.enabled == 1 && value.positional == 1 &&
    std::isfinite(value.position[0]) && std::isfinite(value.position[1]) &&
    std::isfinite(value.position[2]) &&
    std::abs(value.position[0]) <= 1e7f && std::abs(value.position[1]) <= 1e7f &&
    std::abs(value.position[2]) <= 1e7f &&
    std::isfinite(value.directIntensity) && value.directIntensity > 0 &&
    value.directIntensity <= 64 && (value.directColor & 0x00ffffffu);
}

// Process-monotonic identity. Exhaustion is permanent, never wrap to old leases.
struct Generation {
  uint64_t value = 0;
  uint64_t next() noexcept {
    if (value == UINT64_MAX) return 0;
    return ++value;
  }
};

// Reconstruct a guarded hook prefix from a pinned PE32 image, applying only
// IMAGE_REL_BASED_HIGHLOW records. ASLR is not an identity mismatch and must
// not be worked around by disabling relocation in the user's process.
inline bool RelocatedEntry(const uint8_t* file, size_t size, uint32_t actualBase,
    uint32_t rva, std::array<uint8_t, 8>& out) noexcept {
  if (!file || size < 0x100) return false;
  auto u16 = [file](size_t p) { uint16_t v; std::memcpy(&v,file+p,2); return v; };
  auto u32 = [file](size_t p) { uint32_t v; std::memcpy(&v,file+p,4); return v; };
  if (u16(0) != 0x5a4d) return false;
  const uint32_t pe = u32(0x3c);
  if (pe > size-0x100 || u32(pe) != 0x4550 || u16(pe+4) != 0x14c ||
      u16(pe+24) != 0x10b) return false;
  const uint32_t opt = pe+24, preferred = u32(opt+28);
  const uint32_t sections = u16(pe+6), start = opt+u16(pe+20);
  if (!sections || sections > 96 || start > size || sections*40u > size-start ||
      u32(opt+92) < 6 || rva > UINT32_MAX-8) return false;
  auto offset = [&](uint32_t address, uint32_t bytes, size_t& result) {
    for (uint32_t i=0; i<sections; ++i) {
      const uint32_t s = start+i*40, va = u32(s+12), raw = u32(s+16), pos = u32(s+20);
      if (address >= va && address-va <= raw && bytes <= raw-(address-va) &&
          pos <= size && address-va <= size-pos && bytes <= size-pos-(address-va)) {
        result = pos+(address-va); return true;
      }
    }
    return false;
  };
  size_t entry = 0;
  if (!offset(rva, 8, entry)) return false;
  std::memcpy(out.data(), file+entry, 8);
  const uint32_t relocRva = u32(opt+96+5*8), relocSize = u32(opt+100+5*8);
  size_t reloc = 0;
  if (!relocRva || relocSize < 8 || !offset(relocRva, relocSize, reloc)) return false;
  const size_t end = reloc+relocSize;
  while (reloc < end) {
    if (end-reloc < 8) return false;
    const uint32_t page = u32(reloc), length = u32(reloc+4);
    if (length < 8 || length > end-reloc || length%2) return false;
    for (size_t p=reloc+8; p<reloc+length; p+=2) {
      const uint16_t record = u16(p), type = record>>12;
      if (!type) continue;
      if (type != 3 || page > UINT32_MAX-(record&4095)) return false;
      const uint32_t target = page+(record&4095);
      if (uint64_t(target)+4 <= rva || target >= rva+8) continue;
      size_t raw = 0;
      if (!offset(target,4,raw)) return false;
      const uint32_t adjusted = u32(raw)+(actualBase-preferred);
      uint8_t adjustedBytes[4]; std::memcpy(adjustedBytes,&adjusted,4);
      for (uint32_t j=0; j<4; ++j)
        if (uint64_t(target)+j >= rva && uint64_t(target)+j < uint64_t(rva)+8)
          out[target+j-rva] = adjustedBytes[j];
    }
    reloc += length;
  }
  return true;
}
struct Sample {
  Source source;
  NativeValue value;
  uint64_t generation = 0, epoch = 0, frame = 0;
  uint32_t nativeLight = 0;
  uint32_t nativeModel = 0; // render-owner identity only, never dereferenced by a worker
  uint64_t policyGeneration = 0;
  bool explicitRegistration = false; // priority hint; policy generation still gates the lease
};

} // namespace dxvk::war3::native_light
