#include "../src/d3d9/war3/hooks/war3_native_frame_sync_policy.h"
#include <cassert>
#include <array>
int main() {
  using dxvk::war3::hooks::CanElideNativeFrameSync;
  for (int enable=0;enable<2;++enable)
  for (int recording=0;recording<2;++recording)
  for (int readable=0;readable<2;++readable)
  for (int nested=0;nested<2;++nested)
  for (int request=-2;request<4;++request)
  for (unsigned capture=0;capture<3;++capture)
    assert(CanElideNativeFrameSync(enable,recording,readable,nested,request,capture)==
        (enable==1 && recording==1 && readable==1 && nested==0 && request==1 && capture==0));
  using dxvk::war3::hooks::NormalizeNativeFrameCode;
  for (uint32_t base:{0x6F000000u,0x5D120000u,0x72000000u}) {
    std::array<uint8_t,16> code{};
    const uint32_t original=0x6F123456u,delta=base-0x6F000000u,relocated=original+delta;
    std::memcpy(code.data()+3,&relocated,4);
    const size_t offsets[]={3};
    assert(NormalizeNativeFrameCode(code.data(),code.size(),delta,offsets,1));
    uint32_t result;std::memcpy(&result,code.data()+3,4);assert(result==original);
    const size_t invalid[]={14};
    assert(!NormalizeNativeFrameCode(code.data(),code.size(),delta,invalid,1));
    const size_t overlap[]={3,4};
    assert(!NormalizeNativeFrameCode(code.data(),code.size(),delta,overlap,2));
  }
}
