#include "../../tools/war3_frame_timeline_core.h"
#include <cassert>
#include <numeric>
#include <iostream>
using namespace dxvk::war3::timeline;
void closed(const Slice& s) {
  assert(std::accumulate(s.self.begin(),s.self.end(),uint64_t(0))==s.end-s.start);
  assert(std::accumulate(s.legacySelf.begin(),s.legacySelf.end(),uint64_t(0))==s.legacyTicks);
}
int main() {
  Ledger x; x.cut(100); x.legacy(true,110);
  auto a=x.enter(8,120), b=x.enter(2,140);
  auto first=x.cut(170); closed(first);
  assert(first.self[0]==20 && first.self[8]==20 && first.self[2]==30);
  assert(first.legacyTicks==60 && first.inclusive[8]==50);
  x.leave(b,190); x.legacy(false,195); x.leave(a,200);
  auto second=x.cut(210); closed(second);
  assert(second.self[2]==20 && second.self[8]==10 && second.self[0]==10);
  assert(second.legacyTicks==25 && !second.faults);
  auto c=x.enter(8,215), d=x.enter(8,220); x.leave(d,230);x.leave(c,240);
  auto recursive=x.cut(245); closed(recursive);assert(recursive.inclusive[8]==25);
  x.leave(0,249); auto valid=x.cut(250);closed(valid);assert(!valid.faults);
  auto e=x.enter(1,255);x.leave(e+1,260);auto fault=x.cut(280);closed(fault);assert(fault.faults);
  auto persistent=x.cut(300);closed(persistent);assert(persistent.faults && persistent.self[0]==20);
  Ledger overflow;overflow.cut(1);for(int i=0;i<65;++i)overflow.enter(1,2+i);
  assert(overflow.cut(100).faults);
  Ledger backwards;backwards.cut(100);backwards.enter(1,90);assert(backwards.cut(110).faults);
  Ledger high;high.cut(1);auto h=high.enter(33,2);auto h2=high.enter(33,3);high.leave(h2,4);high.leave(h,5);
  auto hi=high.cut(6);closed(hi);assert(hi.inclusive[33]==3 && hi.self[33]==3 && hi.self[0]==2);
  std::cout<<"PASS cross-frame, legacy intersection, nesting, recursion union, no-op, persistent fail-closed, capacity, clock reversal\n";
}
