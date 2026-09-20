#include "../src/d3d9/war3/tools/war3_frame_inputs_core.h"
#include <cassert>
#include <iostream>
using namespace dxvk::war3::tools::evidence::inputs;
int main(){
  assert(contains(100,10,100,10));assert(!contains(100,10,99,1));assert(!contains(100,10,110,1));
  assert(!contains(100,10,100,0));assert(!contains(UINT64_MAX-4,4,UINT64_MAX-1,2));
  uint32_t used=0,offset=0;assert(reserve(used,3,offset)&&offset==0&&used==3);
  assert(reserve(used,4,offset)&&offset==4&&used==8);
  used=BytesPerSlot-1;assert(!reserve(used,1,offset));assert(!reserve(used,UINT64_MAX,offset));
  assert(focus(11,52,105));assert(focus(11,614,1587));assert(!focus(12,52,105));
  std::cout<<"12 bounded input core checks PASS\n";
}
