#pragma once
#include "../d3d9_factory_wire.h"
#include "../../../src/d3d9/d3d9_adapter.h"
#include "../../../src/dxvk/dxvk_instance.h"
namespace warvk::host::dxvkcore {
// One metadata owner per authenticated connection. initialize reuses this
// instance/adapter; querying caps never constructs a Vulkan device or targets.
class D3D9Factory final {
public:
  D3D9Factory();
  HRESULT query(const d9::Header& h,uint8_t* output,uint32_t& outputBytes);
  const dxvk::Rc<dxvk::DxvkInstance>& instance() const { return m_instance; }
  const dxvk::Rc<dxvk::DxvkAdapter>& adapter() const { return m_adapter; }
private:
  dxvk::Rc<dxvk::DxvkInstance> m_instance;
  dxvk::Rc<dxvk::DxvkAdapter> m_adapter;
  std::unique_ptr<dxvk::D3D9Adapter> m_policy;
};
}
