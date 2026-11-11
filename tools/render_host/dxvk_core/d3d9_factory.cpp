#include "d3d9_factory.h"
namespace warvk::host::dxvkcore {
using namespace dxvk;
D3D9Factory::D3D9Factory() {
  m_instance=new DxvkInstance(DxvkInstanceFlag::ClientApiIsD3D9);
  m_adapter=m_instance->enumAdapters(0);
  if(!m_adapter)throw std::runtime_error("D3D9 factory found no physical adapter");
  std::string why;
  if(!m_adapter->isCompatible(why))throw std::runtime_error("D3D9 factory adapter incompatible: "+why);
  if(m_adapter->deviceProperties().core.properties.deviceType==VK_PHYSICAL_DEVICE_TYPE_CPU)
    throw std::runtime_error("D3D9 factory requires hardware adapter");
  m_policy=std::make_unique<D3D9Adapter>(D3D9Options(nullptr,m_instance->config()),false,false,m_adapter,0,0);
}
HRESULT D3D9Factory::query(const d9::Header& h,uint8_t* output,uint32_t& outputBytes) {
  outputBytes=0;
  if(!output||!d9::ValidFactoryQuery(h))return D3DERR_INVALIDCALL;
  using Q=d9::FactoryQuery;
  const auto query=Q(h.args[0]);const auto& a=h.args;
  const auto format=[](uint32_t value){return EnumerateFormat(D3DFORMAT(value));};
  if((query==Q::Caps||(query>=Q::DeviceType&&query<=Q::Conversion))&&a[1]!=D3DDEVTYPE_HAL)
    return D3DERR_INVALIDCALL;
  HRESULT hr=D3DERR_INVALIDCALL;
  switch(query) {
    case Q::Caps: {
      D3DCAPS9 caps{};hr=m_policy->GetDeviceCaps(D3DDEVTYPE_HAL,&caps);
      if(hr==D3D_OK)d9::EncodeCaps(caps,output);break;
    }
    case Q::Identifier: {
      D3DADAPTER_IDENTIFIER9 id{};hr=m_policy->GetAdapterIdentifier(a[1],&id);
      if(hr==D3D_OK)d9::EncodeIdentifier(id,output);break;
    }
    case Q::DeviceType:
      if(a[4]>1)return D3DERR_INVALIDCALL;
      hr=m_policy->CheckDeviceType(D3DDEVTYPE_HAL,format(a[2]),format(a[3]),BOOL(a[4]));break;
    case Q::Format:
      hr=m_policy->CheckDeviceFormat(D3DDEVTYPE_HAL,format(a[2]),a[3],D3DRESOURCETYPE(a[4]),format(a[5]));break;
    case Q::Multisample: {
      if(a[3]>1)return D3DERR_INVALIDCALL;
      DWORD quality=0;hr=m_policy->CheckDeviceMultiSampleType(D3DDEVTYPE_HAL,format(a[2]),BOOL(a[3]),D3DMULTISAMPLE_TYPE(a[4]),&quality);
      if(hr==D3D_OK)d9::P32(output,quality);break;
    }
    case Q::DepthMatch:
      hr=m_policy->CheckDepthStencilMatch(D3DDEVTYPE_HAL,format(a[2]),format(a[3]),format(a[4]));break;
    case Q::Conversion:
      hr=m_policy->CheckDeviceFormatConversion(D3DDEVTYPE_HAL,format(a[2]),format(a[3]));break;
    case Q::ModeCount:case Q::EnumMode: {
      D3DDISPLAYMODEFILTER filter{sizeof(D3DDISPLAYMODEFILTER),D3DFORMAT(a[1]),D3DSCANLINEORDERING_PROGRESSIVE};
      if(query==Q::ModeCount) {d9::P32(output,m_policy->GetAdapterModeCountEx(&filter));hr=D3D_OK;break;}
      D3DDISPLAYMODEEX mode{};mode.Size=sizeof(mode);hr=m_policy->EnumAdapterModesEx(&filter,a[2],&mode);
      if(hr==D3D_OK){d9::P32(output,mode.Width);d9::P32(output+4,mode.Height);d9::P32(output+8,mode.RefreshRate);d9::P32(output+12,mode.Format);}
      break;
    }
    case Q::DisplayMode: {
      D3DDISPLAYMODEEX mode{};mode.Size=sizeof(mode);hr=m_policy->GetAdapterDisplayModeEx(&mode,nullptr);
      if(hr==D3D_OK){d9::P32(output,mode.Width);d9::P32(output+4,mode.Height);d9::P32(output+8,mode.RefreshRate);d9::P32(output+12,mode.Format);}
      break;
    }
  }
  if(hr==D3D_OK)outputBytes=d9::FactoryResultBytes(query);
  return hr;
}
}
