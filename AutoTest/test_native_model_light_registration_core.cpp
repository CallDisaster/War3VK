#include "../src/d3d9/war3/model/war3_native_light_core.h"
#include <cassert>
#include <vector>
#include <fstream>
#include <iostream>
using namespace dxvk::war3::native_light;
void put(std::vector<uint8_t>& b,size_t p,uint32_t v) { std::memcpy(b.data()+p,&v,4); }
std::vector<uint8_t> fixture(uint32_t count=2) {
  std::vector<uint8_t> b(24+144*count);
  std::memcpy(b.data(),"MDLXVERS",8); put(b,8,4); put(b,12,800);
  std::memcpy(b.data()+16,"LITE",4); put(b,20,144*count);
  for (uint32_t i=0;i<count;++i) {
    size_t p=24+i*144; put(b,p,144); put(b,p+4,96); put(b,p+88,i+2); put(b,p+92,UINT32_MAX);
    float start=80,range=200; std::memcpy(b.data()+p+104,&start,4); std::memcpy(b.data()+p+108,&range,4);
  }
  return b;
}
int main(int argc,char** argv) {
  std::string normalized;
  assert(NormalizeModelLightPath("war3mapImported/TorchHuman.MDL",normalized));
  assert(normalized=="war3mapimported\\torchhuman.mdx");
  for (auto p:{"","C:\\torch.mdx","\\torch.mdx","../torch.mdx","a\\..\\x.mdx","a\\\\x.mdx","a;evil.mdx","a.txt"})
    assert(!NormalizeModelLightPath(p,normalized));
  assert(!NormalizeModelLightPath(std::string(241,'a'),normalized));
  ModelLightSources source;
  auto b=fixture(); assert(DecodeModelLightSources(b.data(),b.size(),source));
  assert(source.count==2 && source.lights[0].ordinal==0 && source.lights[1].ordinal==1 && source.lights[1].count==2);
  assert(source.lights[0].objectId==2 && source.lights[1].objectId==3 && source.lights[1].range==200);
  assert(!DecodeModelLightSources(b.data(),b.size()-1,source));
  auto bad=b; put(bad,24+144+88,2); assert(!DecodeModelLightSources(bad.data(),bad.size(),source));
  bad=b; put(bad,12,1000); assert(!DecodeModelLightSources(bad.data(),bad.size(),source));
  bad=b; put(bad,24,UINT32_MAX); assert(!DecodeModelLightSources(bad.data(),bad.size(),source));
  bad=b; put(bad,24+4,UINT32_MAX); assert(!DecodeModelLightSources(bad.data(),bad.size(),source));
  bad=b; put(bad,24+108,0x7fc00000); assert(DecodeModelLightSources(bad.data(),bad.size(),source) && source.count==1);
  bad=fixture(17); assert(!DecodeModelLightSources(bad.data(),bad.size(),source));
  bad=fixture(1); put(bad,24+100,1); assert(!DecodeModelLightSources(bad.data(),bad.size(),source)); // directional
  bad=fixture(1); bad.resize(bad.size()+20); put(bad,20,164); put(bad,24,164);
  std::memcpy(bad.data()+168,"KLAS",4); put(bad,172,1); put(bad,176,0);
  assert(!DecodeModelLightSources(bad.data(),bad.size(),source)); // animated range must not seed static range
  for (int i=1;i<argc;++i) {
    std::ifstream f(argv[i],std::ios::binary); std::vector<uint8_t> real((std::istreambuf_iterator<char>(f)),{});
    assert(DecodeModelLightSources(real.data(),real.size(),source));
    assert(source.count==1 && source.lights[0].objectId==2 && source.lights[0].range==200);
  }
  std::cout<<"model light path/parser cases passed; real="<<argc-1<<'\n';
}
