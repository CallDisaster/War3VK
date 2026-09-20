#include "../src/d3d9/war3/tools/war3_frame_history_core.h"
#include <iostream>
#include <stdexcept>
using namespace dxvk::war3::tools::history;
int main(){unsigned checks=0;auto check=[&](bool v){++checks;if(!v)throw std::runtime_error("history core");};
  try{uint64_t bytes=1;check(Budget(2560,1440,64,4,bytes));check(bytes==1002700800);
    check(Budget(2560,1440,256,4,bytes));check(bytes==3833856000ull);
    check(!Budget(3840,2160,256,4,bytes));check(bytes==0);check(!Budget(0,1440,64,4,bytes));
    check(!Budget(2560,1440,257,4,bytes));check(!Budget(2560,1440,64,17,bytes));
    check(!Budget(UINT32_MAX,UINT32_MAX,4,2,bytes));
    for(uint32_t pre=4;pre<=256;++pre)for(uint32_t n=0;n<pre*3;++n){
      uint32_t filled=n<pre?n:pre,head=n%pre;std::array<uint32_t,MaxSlots> order{};
      check(Gather(pre,filled,head,4,order)==filled+4);
      for(uint32_t i=0;i<filled;++i)check(order[i]==(n-filled+i)%pre);
      for(uint32_t i=0;i<4;++i)check(order[filled+i]==pre+i);
    }
    check(IsShortcut(0x100,'C',true,true,false));check(!IsShortcut(0x100,0x77,true,true,false));
    check(!IsShortcut(0x100,'C',false,true,false));check(!IsShortcut(0x100,'C',true,false,false));
    check(!IsShortcut(0x100,'C',true,true,true));check(!IsShortcut(0x101,'C',true,true,false));
    std::cout<<"history budget/order checks="<<checks<<" PASS\n";return 0;
  }catch(const std::exception& e){std::cerr<<e.what();return 1;}}
