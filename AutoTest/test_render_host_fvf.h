// Frozen pre-extraction production interpretation (2026-10-29).
// Kept independent of the new shared decoder for differential regressions.
#pragma once
#include "../tools/render_host/d3d9_declaration_wire.h"
#include <vector>
#include <cstring>
namespace warvk_fvf_test {
struct Original {
 DWORD m_fvf=0;
 std::vector<D3DVERTEXELEMENT9> m_elements;
   uint32_t GetDecltypeSize(D3DDECLTYPE Type) {
    switch (Type) {
      case D3DDECLTYPE_FLOAT1:    return 1 * sizeof(float);
      case D3DDECLTYPE_FLOAT2:    return 2 * sizeof(float);
      case D3DDECLTYPE_FLOAT3:    return 3 * sizeof(float);
      case D3DDECLTYPE_FLOAT4:    return 4 * sizeof(float);
      case D3DDECLTYPE_D3DCOLOR:  return 1 * sizeof(DWORD);
      case D3DDECLTYPE_UBYTE4:    return 4 * sizeof(BYTE);
      case D3DDECLTYPE_SHORT2:    return 2 * sizeof(short);
      case D3DDECLTYPE_SHORT4:    return 4 * sizeof(short);
      case D3DDECLTYPE_UBYTE4N:   return 4 * sizeof(BYTE);
      case D3DDECLTYPE_SHORT2N:   return 2 * sizeof(short);
      case D3DDECLTYPE_SHORT4N:   return 4 * sizeof(short);
      case D3DDECLTYPE_USHORT2N:  return 2 * sizeof(short);
      case D3DDECLTYPE_USHORT4N:  return 4 * sizeof(short);
      case D3DDECLTYPE_UDEC3:     return 4;
      case D3DDECLTYPE_DEC3N:     return 4;
      case D3DDECLTYPE_FLOAT16_2: return 2 * 2;
      case D3DDECLTYPE_FLOAT16_4: return 4 * 2;
      default:                    return 0;
    }
  }
   void SetFVF(DWORD FVF) {
    m_fvf = FVF;

    std::array<D3DVERTEXELEMENT9, 16> elements;
    uint32_t elemCount = 0;
    uint32_t texCount = 0;

    uint32_t betas = 0;
    uint8_t betaIdx = 0xFF;

    switch (FVF & D3DFVF_POSITION_MASK) {
      case D3DFVF_XYZ:
      case D3DFVF_XYZB1:
      case D3DFVF_XYZB2:
      case D3DFVF_XYZB3:
      case D3DFVF_XYZB4:
      case D3DFVF_XYZB5:
        elements[elemCount].Type = D3DDECLTYPE_FLOAT3;
        elements[elemCount].Usage = D3DDECLUSAGE_POSITION;
        elements[elemCount].UsageIndex = 0;
        elemCount++;

        if ((FVF & D3DFVF_POSITION_MASK) == D3DFVF_XYZ)
          break;

        betas = (((FVF & D3DFVF_XYZB5) - D3DFVF_XYZB1) >> 1) + 1;
        if (FVF & D3DFVF_LASTBETA_D3DCOLOR)
          betaIdx = D3DDECLTYPE_D3DCOLOR;
        else if (FVF & D3DFVF_LASTBETA_UBYTE4)
          betaIdx = D3DDECLTYPE_UBYTE4;
        else if ((FVF & D3DFVF_XYZB5) == D3DFVF_XYZB5)
          betaIdx = D3DDECLTYPE_FLOAT1;

        if (betaIdx != 0xFF)
          betas--;

        if (betas > 0) {
          switch (betas) {
            case 1: elements[elemCount].Type = D3DDECLTYPE_FLOAT1; break;
            case 2: elements[elemCount].Type = D3DDECLTYPE_FLOAT2; break;
            case 3: elements[elemCount].Type = D3DDECLTYPE_FLOAT3; break;
            case 4: elements[elemCount].Type = D3DDECLTYPE_FLOAT4; break;
            default: break;
          }
          elements[elemCount].Usage = D3DDECLUSAGE_BLENDWEIGHT;
          elements[elemCount].UsageIndex = 0;
          elemCount++;
        }

        if (betaIdx != 0xFF) {
          elements[elemCount].Type = betaIdx;
          elements[elemCount].Usage = D3DDECLUSAGE_BLENDINDICES;
          elements[elemCount].UsageIndex = 0;
          elemCount++;
        }
        break;

      case D3DFVF_XYZW:
      case D3DFVF_XYZRHW:
        elements[elemCount].Type = D3DDECLTYPE_FLOAT4;
        elements[elemCount].Usage =
          ((FVF & D3DFVF_POSITION_MASK) == D3DFVF_XYZW)
          ? D3DDECLUSAGE_POSITION
          : D3DDECLUSAGE_POSITIONT;
        elements[elemCount].UsageIndex = 0;
        elemCount++;
        break;

      default:
        break;
    }

    if (FVF & D3DFVF_NORMAL) {
      elements[elemCount].Type = D3DDECLTYPE_FLOAT3;
      elements[elemCount].Usage = D3DDECLUSAGE_NORMAL;
      elements[elemCount].UsageIndex = 0;
      elemCount++;
    }
    if (FVF & D3DFVF_PSIZE) {
      elements[elemCount].Type = D3DDECLTYPE_FLOAT1;
      elements[elemCount].Usage = D3DDECLUSAGE_PSIZE;
      elements[elemCount].UsageIndex = 0;
      elemCount++;
    }
    if (FVF & D3DFVF_DIFFUSE) {
      elements[elemCount].Type = D3DDECLTYPE_D3DCOLOR;
      elements[elemCount].Usage = D3DDECLUSAGE_COLOR;
      elements[elemCount].UsageIndex = 0;
      elemCount++;
    }
    if (FVF & D3DFVF_SPECULAR) {
      elements[elemCount].Type = D3DDECLTYPE_D3DCOLOR;
      elements[elemCount].Usage = D3DDECLUSAGE_COLOR;
      elements[elemCount].UsageIndex = 1;
      elemCount++;
    }

    texCount = (FVF & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
    texCount = std::min(texCount, 8u);

    for (uint32_t i = 0; i < texCount; i++) {
      switch ((FVF >> (16 + i * 2)) & 0x3) {
        case D3DFVF_TEXTUREFORMAT1:
          elements[elemCount].Type = D3DDECLTYPE_FLOAT1;
          break;

        case D3DFVF_TEXTUREFORMAT2:
          elements[elemCount].Type = D3DDECLTYPE_FLOAT2;
          break;

        case D3DFVF_TEXTUREFORMAT3:
          elements[elemCount].Type = D3DDECLTYPE_FLOAT3;
          break;

        case D3DFVF_TEXTUREFORMAT4:
          elements[elemCount].Type = D3DDECLTYPE_FLOAT4;
          break;

        default:
          break;
      }
      elements[elemCount].Usage = D3DDECLUSAGE_TEXCOORD;
      elements[elemCount].UsageIndex = i;
      elemCount++;
    }

    for (uint32_t i = 0; i < elemCount; i++) {
      elements[i].Stream = 0;
      elements[i].Offset = (i == 0)
        ? 0
        : (elements[i - 1].Offset + GetDecltypeSize(D3DDECLTYPE(elements[i - 1].Type)));

      elements[i].Method = D3DDECLMETHOD_DEFAULT;
    }

    m_elements.resize(elemCount);
    std::copy(elements.begin(), elements.begin() + elemCount, m_elements.data());
  }
   bool MapD3DDeclToFvf(
    const D3DVERTEXELEMENT9& element,
          DWORD fvf,
          DWORD& outFvf,
          DWORD& texCountPostUpdate) {

    // Mapping between a Direct3D Declaration and FVF Codes (Direct3D 9)
    // This table maps members of a D3DVERTEXELEMENT9 declaration to a FVF code.
    //
    // Data type              Usage                       Usage index     FVF
    // ----------------------------------------------------------------------------------------
    // D3DDECLTYPE_FLOAT3     D3DDECLUSAGE_POSITION       0               D3DFVF_XYZ
    // D3DDECLTYPE_FLOAT4     D3DDECLUSAGE_POSITIONT      0               D3DFVF_XYZRHW
    // D3DDECLTYPE_FLOATn     D3DDECLUSAGE_BLENDWEIGHT    0               D3DFVF_XYZBn
    // D3DDECLTYPE_UBYTE4     D3DDECLUSAGE_BLENDINDICES   0               D3DFVF_XYZB(nWeights + 1)
    // D3DDECLTYPE_FLOAT3     D3DDECLUSAGE_NORMAL         0               D3DFVF_NORMAL
    // D3DDECLTYPE_FLOAT1     D3DDECLUSAGE_PSIZE          0               D3DFVF_PSIZE
    // D3DDECLTYPE_D3DCOLOR   D3DDECLUSAGE_COLOR          0               D3DFVF_DIFFUSE
    // D3DDECLTYPE_D3DCOLOR   D3DDECLUSAGE_COLOR          1               D3DFVF_SPECULAR
    // D3DDECLTYPE_FLOATm     D3DDECLUSAGE_TEXCOORD       n               D3DFVF_TEXCOORDSIZEm(n)
    // D3DDECLTYPE_FLOAT3     D3DDECLUSAGE_POSITION       1               N / A
    // D3DDECLTYPE_FLOAT3     D3DDECLUSAGE_NORMAL         1               N / A


    if (element.Usage == D3DDECLUSAGE_POSITION && element.Type == D3DDECLTYPE_FLOAT3 && element.UsageIndex == 0) {
      outFvf = D3DFVF_XYZ;
      return true;
    }

    if (element.Usage == D3DDECLUSAGE_POSITIONT && element.Type == D3DDECLTYPE_FLOAT4 && element.UsageIndex == 0) {
      outFvf = D3DFVF_XYZRHW;
      return true;
    }

    if (element.Usage == D3DDECLUSAGE_BLENDWEIGHT && element.UsageIndex == 0) {
      DWORD fvfRet = MapD3DDeclTypeFloatToFvfXYZBn(element.Type);
      if (fvfRet != 0) {
        outFvf = fvfRet;
        return true;
      } else {
        return false;
      }
    }

    if (element.Usage == D3DDECLUSAGE_BLENDINDICES && element.Type == D3DDECLTYPE_UBYTE4 && element.UsageIndex == 0) {
      outFvf = D3DFVF_XYZB1;
      return true;
    }

    if (element.Usage == D3DDECLUSAGE_NORMAL && element.Type == D3DDECLTYPE_FLOAT3 && element.UsageIndex == 0) {
      outFvf = D3DFVF_NORMAL;
      return true;
    }

    if (element.Usage == D3DDECLUSAGE_PSIZE && element.Type == D3DDECLTYPE_FLOAT1 && element.UsageIndex == 0) {
      outFvf = D3DFVF_PSIZE;
      return true;
    }

    if (element.Usage == D3DDECLUSAGE_COLOR && element.Type == D3DDECLTYPE_D3DCOLOR) {
      switch (element.UsageIndex) {
        case 0:
          outFvf = D3DFVF_DIFFUSE;
          return true;
        case 1:
          outFvf = D3DFVF_SPECULAR;
          return true;
        default:
          return false;
      }
    }

    if (element.Usage == D3DDECLUSAGE_TEXCOORD && element.UsageIndex < 8) {
      return MapD3DDeclUsageTexCoordToFvfTexCoordSize(element, fvf, outFvf, texCountPostUpdate);
    }

    return false;
  }


  DWORD MapD3DDeclTypeFloatToFvfXYZBn(BYTE type) {

    switch (type) {
      case D3DDECLTYPE_FLOAT1: return D3DFVF_XYZB1;
      case D3DDECLTYPE_FLOAT2: return D3DFVF_XYZB2;
      case D3DDECLTYPE_FLOAT3: return D3DFVF_XYZB3;
      case D3DDECLTYPE_FLOAT4: return D3DFVF_XYZB4;
      default:                 return 0;
    }
  }


  bool MapD3DDeclUsageTexCoordToFvfTexCoordSize(
      const D3DVERTEXELEMENT9& element,
      DWORD fvf,
      DWORD& outFvf,
      DWORD& texCountPostUpdate) {

    // Check if bits of format for current UsageIndex are free in the fvf
    // It is necessary to skip multiple initializations of the bitfield because
    // returned value is bitwise or-ed to final fvf DWORD.
    // The D3DFVF_TEXCOORDSIZE1 is used below because it covers all formats bits.
    if ((D3DFVF_TEXCOORDSIZE1(element.UsageIndex) & fvf) != 0)
      return false;

    // Update max texture's index in fvf
    DWORD currentTexCount = element.UsageIndex + 1;
    bool retStatus = true;

    if (texCountPostUpdate < currentTexCount)
      texCountPostUpdate = currentTexCount;

    if (element.Type == D3DDECLTYPE_FLOAT1)
      outFvf = D3DFVF_TEXCOORDSIZE1(element.UsageIndex);
    else if (element.Type == D3DDECLTYPE_FLOAT2)
      outFvf = D3DFVF_TEXCOORDSIZE2(element.UsageIndex);
    else if (element.Type == D3DDECLTYPE_FLOAT3)
      outFvf = D3DFVF_TEXCOORDSIZE3(element.UsageIndex);
    else if (element.Type == D3DDECLTYPE_FLOAT4)
      outFvf = D3DFVF_TEXCOORDSIZE4(element.UsageIndex);
    else
      retStatus = false;

    return retStatus;
  }


  DWORD MapD3D9VertexElementsToFvf() {
    DWORD fvf = 0;
    DWORD texCountPostUpdate = 0;

    for (const auto& element : m_elements) {
      DWORD elementFvf = 0;
      if (!MapD3DDeclToFvf(element, fvf, elementFvf, texCountPostUpdate)) {
        return 0;
      }
      fvf |= elementFvf;
    }

    fvf |= (texCountPostUpdate << 8);

    return fvf;
  }
};
template<class Check> void Run(Check check) {
  uint32_t seed=0x91ea4a27u;
  Original old;
  for(unsigned n=0;n<131072;++n) {
    seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;
    // All 16 low-bit combinations crossed with all four texcoord widths,
    // then independent high-bit pseudorandom combinations.
    const DWORD fvf=n<65536?n:seed;
    old.SetFVF(fvf);const auto fresh=dxvk::DecodeFvf(fvf);
    check(fresh.count==old.m_elements.size());
    check(!std::memcmp(fresh.elements.data(),old.m_elements.data(),fresh.count*sizeof(D3DVERTEXELEMENT9)));
    check(dxvk::FvfMapping::MapD3D9VertexElementsToFvf(fresh.elements.data(),fresh.count)==old.MapD3D9VertexElementsToFvf());
  }
  for(uint32_t type=0;type<256;++type)check(dxvk::DecltypeSize(D3DDECLTYPE(type))==old.GetDecltypeSize(D3DDECLTYPE(type)));
  const auto compact=dxvk::DecodeFvf(D3DFVF_XYZ|D3DFVF_NORMAL|D3DFVF_DIFFUSE|D3DFVF_TEX2|D3DFVF_TEXCOORDSIZE1(1));
  const D3DVERTEXELEMENT9 expected[]={{0,0,D3DDECLTYPE_FLOAT3,0,D3DDECLUSAGE_POSITION,0},
    {0,12,D3DDECLTYPE_FLOAT3,0,D3DDECLUSAGE_NORMAL,0},{0,24,D3DDECLTYPE_D3DCOLOR,0,D3DDECLUSAGE_COLOR,0},
    {0,28,D3DDECLTYPE_FLOAT2,0,D3DDECLUSAGE_TEXCOORD,0},{0,36,D3DDECLTYPE_FLOAT1,0,D3DDECLUSAGE_TEXCOORD,1}};
  check(compact.count==5&&!std::memcmp(compact.elements.data(),expected,sizeof(expected)));
  using namespace warvk::host;
  std::array<uint8_t,128> bytes{};
  for(uint32_t i=0;i<compact.count;++i)d9::EncodeDeclarationElement(bytes.data()+8*i,compact.elements[i]);
  d9::DeclarationCommand decoded;
  check(d9::DecodeDeclaration(bytes.data(),40,decoded)&&decoded.extent==40&&decoded.count==5);
  for(uint32_t len=0;len<40;++len) {
    auto out=decoded;const auto before=out;
    const bool valid=d9::DecodeDeclaration(bytes.data(),len,out);
    check(valid==(len!=0&&len%8==0));
    if(!valid)check(!std::memcmp(&out,&before,sizeof(out)));
  }
  for(uint32_t i=0;i<5;++i)for(unsigned field:{0u,2u,4u,5u,6u,7u}) {
    auto bad=bytes;bad[i*8+field]=255;
    auto out=decoded;check(!d9::DecodeDeclaration(bad.data(),40,out));check(!std::memcmp(&out,&decoded,sizeof(out)));
  }
  auto duplicate=bytes;duplicate[8+6]=D3DDECLUSAGE_POSITION;
  check(!d9::DecodeDeclaration(duplicate.data(),40,decoded));
  check(!d9::DecodeDeclaration(nullptr,40,decoded));
  check(!d9::DecodeDeclaration(bytes.data(),136,decoded));
}
}

