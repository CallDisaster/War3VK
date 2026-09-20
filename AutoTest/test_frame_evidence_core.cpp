#include "../src/d3d9/war3/tools/war3_frame_evidence_core.h"
#include <deque>
#include <iostream>
#include <stdexcept>

using namespace dxvk::war3::tools::evidence;
static unsigned checks=0;
#define CHECK(x) do {++checks; if(!(x)) throw std::runtime_error(#x);} while(false)
int main() {
  try {
    Ring r; Event e{};
    CHECK(!r.arm(0,8)); CHECK(!r.arm(1,0)); CHECK(!r.arm(1,262145));
    CHECK(!r.append(1,e)); CHECK(!r.finish(1)); CHECK(r.arm(1,8));
    CHECK(!r.arm(2,8)); CHECK(!r.trigger(2,1)); CHECK(!r.trigger(1,121));
    for(unsigned i=1;i<=20;++i) {e.data[0]=i;CHECK(r.append(1,e)==i);}
    auto s=r.snapshot(true); CHECK(s.events.size()==6); CHECK(s.evicted==14);
    CHECK(s.events.front().sequence==15); CHECK(s.events.back().data[0]==20);
    CHECK(r.trigger(1,1)); CHECK(!r.discard(1));
    CHECK(r.append(1,e)==21); e.kind=Kind::PresentEnd; CHECK(r.append(1,e)==22);
    CHECK(r.state()==State::Frozen); CHECK(r.snapshot(false).reason==Reason::PostWindow);
    CHECK(!r.append(1,e)); CHECK(r.snapshot(true).events.size()==8);
    CHECK(!r.discard(2)); CHECK(r.discard(1)); CHECK(r.arm(2,8));
    CHECK(!r.append(1,e)); CHECK(!r.finish(1)); CHECK(r.finish(2));
    CHECK(r.snapshot(false).reason==Reason::Requested); CHECK(r.discard(2));
    CHECK(r.arm(3,4)); e.kind=Kind::PassBegin;
    for(unsigned i=0;i<3;++i) CHECK(r.append(3,e));
    CHECK(r.trigger(3,2)); CHECK(r.append(3,e)); CHECK(!r.append(3,e));
    CHECK(r.snapshot(false).reason==Reason::Capacity); CHECK(r.discard(3));
    CHECK(r.arm(4,17)); std::deque<uint64_t> expected;
    for(unsigned i=1;i<=2500;++i) {
      e.data[0]=i; CHECK(r.append(4,e)==i); expected.push_back(i);
      if(expected.size()>13) expected.pop_front();
      auto snap=r.snapshot(true); CHECK(snap.events.size()==expected.size());
      for(size_t j=0;j<expected.size();++j) CHECK(snap.events[j].data[0]==expected[j]);
      CHECK(snap.events.size()+snap.evicted==snap.accepted);
    }
    CHECK(r.trigger(4,0)); CHECK(r.state()==State::Frozen); CHECK(!r.finish(4));
    std::cout<<"frame evidence shared core checks="<<checks<<" PASS\n";
  } catch(const std::exception& e) {std::cerr<<e.what()<<"\n";return 1;}
}
