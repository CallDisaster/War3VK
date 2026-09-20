#include "../src/d3d9/war3/model/war3_native_light_core.h"
#include <cassert>
#include <fstream>
#include <vector>
#include <iostream>
int main(int argc, char** argv) {
  assert(argc == 2);
  std::ifstream input(argv[1],std::ios::binary);
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)),{});
  std::array<uint8_t,8> preferred, relocated, plain;
  using dxvk::war3::native_light::RelocatedEntry;
  assert(RelocatedEntry(bytes.data(),bytes.size(),0x6f000000,0xcc530,preferred));
  assert(RelocatedEntry(bytes.data(),bytes.size(),0x72470000,0xcc530,relocated));
  uint32_t a,b; std::memcpy(&a,preferred.data()+2,4); std::memcpy(&b,relocated.data()+2,4);
  assert(a==0x6fbba150 && b==0x7302a150);
  assert(RelocatedEntry(bytes.data(),bytes.size(),0x6f000000,0x48c10,preferred));
  assert(RelocatedEntry(bytes.data(),bytes.size(),0x72470000,0x48c10,plain));
  assert(preferred==plain);
  assert(!RelocatedEntry(bytes.data(),40,0x72470000,0x48c10,plain));
  assert(!RelocatedEntry(bytes.data(),bytes.size(),0x72470000,0xfffffff9,plain));
  std::cout<<"PE ASLR exact entry tests passed\n";
}
