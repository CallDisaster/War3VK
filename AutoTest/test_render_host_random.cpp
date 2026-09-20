#include "../tools/render_host/win32_transport.h"
#include <cstdio>
#include <set>

int main(){
  using namespace warvk::host::lab;
  DWORD baseline=0,first=0,initialized=0,last=0;
  GetProcessHandleCount(GetCurrentProcess(),&baseline);
  std::set<std::wstring> observed;
  for(unsigned i=0;i<72;++i){
    auto nonce=RandomNonce();auto text=NonceText(nonce);
    if(!(ParseNonce(text)==nonce)||!observed.insert(text).second)return 2;
    GetProcessHandleCount(GetCurrentProcess(),&last);
    if(!i)first=last;
    if(i==7)initialized=last;
    if(i>7&&last!=initialized)return 3;
  }
  std::printf("{\"bits\":%u,\"samples\":64,\"initializationSamples\":8,\"handlesBaseline\":%lu,"
    "\"handlesFirst\":%lu,\"handlesInitialized\":%lu,\"handlesLast\":%lu}\n",
    unsigned(sizeof(void*)*8),static_cast<unsigned long>(baseline),
    static_cast<unsigned long>(first),static_cast<unsigned long>(initialized),static_cast<unsigned long>(last));
  return initialized==last?0:1;
}
