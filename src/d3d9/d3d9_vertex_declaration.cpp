#include "d3d9_vertex_declaration.h"
#include "d3d9_fvf.h"
#include "d3d9_util.h"

#include <algorithm>
#include <cstring>

namespace dxvk {

  D3D9VertexDecl::D3D9VertexDecl(
          D3D9DeviceEx*      pDevice,
          DWORD              FVF)
    : D3D9VertexDeclBase(pDevice) {
    this->SetFVF(FVF);
    this->Classify();
  }


  D3D9VertexDecl::D3D9VertexDecl(
          D3D9DeviceEx*      pDevice,
    const D3DVERTEXELEMENT9* pVertexElements,
          uint32_t           DeclCount)
    : D3D9VertexDeclBase( pDevice )
    , m_elements        ( pVertexElements, pVertexElements + DeclCount )
    , m_fvf             ( this->MapD3D9VertexElementsToFvf() ) {
    this->Classify();
  }


  HRESULT STDMETHODCALLTYPE D3D9VertexDecl::QueryInterface(
          REFIID  riid,
          void** ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown)
     || riid == __uuidof(IDirect3DVertexDeclaration9)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(IDirect3DVertexDeclaration9), riid)) {
      Logger::warn("D3D9VertexDecl::QueryInterface: Unknown interface query");
      Logger::warn(str::format(riid));
    }

    return E_NOINTERFACE;
  }


  HRESULT STDMETHODCALLTYPE D3D9VertexDecl::GetDeclaration(
          D3DVERTEXELEMENT9* pElement,
          UINT*              pNumElements) {
    if (pNumElements == nullptr)
      return D3DERR_INVALIDCALL;

    *pNumElements = UINT(m_elements.size()) + 1u; // Account for D3DDECL_END

    if (pElement == nullptr)
      return D3D_OK;

    // The native runtime ignores pNumElements here...
    std::copy(m_elements.begin(), m_elements.end(), pElement);
    pElement[m_elements.size()] = D3DDECL_END();

    return D3D_OK;
  }


  void D3D9VertexDecl::SetFVF(DWORD FVF) {
    m_fvf = FVF;
    const auto decoded = DecodeFvf(FVF);
    m_elements.assign(decoded.elements.begin(), decoded.elements.begin() + decoded.count);
  }

  DWORD D3D9VertexDecl::MapD3D9VertexElementsToFvf() {
    return FvfMapping::MapD3D9VertexElementsToFvf(m_elements.data(), uint32_t(m_elements.size()));
  }


  void D3D9VertexDecl::Classify() {
    for (const auto& element : m_elements) {
      if (element.Type != D3DDECLTYPE_UNUSED)
        m_sizes[element.Stream] = std::max(m_sizes[element.Stream], element.Offset + GetDecltypeSize(D3DDECLTYPE(element.Type)));

      if (element.Usage == D3DDECLUSAGE_COLOR && element.UsageIndex == 0)
        m_flags.set(D3D9VertexDeclFlag::HasColor0);
      else if (element.Usage == D3DDECLUSAGE_COLOR && element.UsageIndex == 1)
        m_flags.set(D3D9VertexDeclFlag::HasColor1);
      else if (element.Usage == D3DDECLUSAGE_POSITIONT)
        m_flags.set(D3D9VertexDeclFlag::HasPositionT);
      else if (element.Usage == D3DDECLUSAGE_PSIZE)
        m_flags.set(D3D9VertexDeclFlag::HasPointSize);
      else if (element.Usage == D3DDECLUSAGE_FOG)
        m_flags.set(D3D9VertexDeclFlag::HasFog);
      else if (element.Usage == D3DDECLUSAGE_BLENDWEIGHT)
        m_flags.set(D3D9VertexDeclFlag::HasBlendWeight);
      else if (element.Usage == D3DDECLUSAGE_BLENDINDICES)
        m_flags.set(D3D9VertexDeclFlag::HasBlendIndices);

      if (element.Usage == D3DDECLUSAGE_TEXCOORD)
        m_texcoordMask |= GetDecltypeCount(D3DDECLTYPE(element.Type)) << (element.UsageIndex * 3);

      m_streamMask |= 1 << element.Stream;
    }
  }

}
