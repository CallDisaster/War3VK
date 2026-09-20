#include "../src/d3d9/war3/tools/war3_frame_recorder_session.h"
#include "../src/d3d9/war3/tools/war3_frame_evidence_core.h"
#include "../src/d3d9/war3/tools/war3_frame_recorder_control.h"
#include <iostream>
#include <stdexcept>
#include <thread>
#include <type_traits>
using namespace dxvk::war3::tools;
unsigned checks=0;
#define CHECK(x) do{++checks;if(!(x))throw std::runtime_error(#x);}while(false)
int main(){try{
  using S=history::RecorderSession;using A=S::Action;
  using I=history::CaptureLifecycle;
  static_assert(I::Idle==0&&I::PendingArm==1&&I::Armed==2&&I::Triggered==3&&
    I::Frozen==4&&I::Exporting==5&&I::Complete==6&&I::Fault==7&&I::Discard==8);
  S s;
  CHECK(!s.resetEndsSession(true));CHECK(!s.resetEndsSession(false));
  for(unsigned i=0;i<100;++i)CHECK(s.next(false,I::Idle,false,false)==A::None);
  CHECK(s.next(true,I::Idle,false,false)==A::Arm);s.armed(7);
  CHECK(s.resetEndsSession(true));CHECK(!s.resetEndsSession(false));
  for(auto state:{I::PendingArm,I::Armed,I::Triggered,I::Frozen,I::Exporting})
    CHECK(s.next(true,state,true,false)==A::None);
  CHECK(s.next(true,I::Complete,false,false)==A::None);
  CHECK(s.next(true,I::Complete,true,false)==A::Export);
  CHECK(s.next(true,I::Complete,true,false)==A::None); // never retries a write
  s.finished(true);CHECK(s.next(true,I::Complete,true,false)==A::Stop);
  for(bool armed:{false,true}){S t;CHECK(t.next(true,I::Idle,false,false)==A::Arm);t.armed(armed?8:0);
    CHECK(t.next(true,I::Fault,true,false)==A::Stop);CHECK(t.next(true,I::Idle,true,false)==A::Stop);}
  S stopped;CHECK(stopped.next(true,I::Idle,true,true)==A::Stop);
  history::TriggerMailbox mailbox;
  CHECK(!mailbox.request());mailbox.arm(7);CHECK(mailbox.request());CHECK(mailbox.request());
  CHECK(mailbox.consume(7));CHECK(!mailbox.consume(7));
  CHECK(mailbox.request());mailbox.disarm();CHECK(!mailbox.request());mailbox.arm(8);
  CHECK(!mailbox.consume(8));CHECK(mailbox.request());CHECK(!mailbox.consume(7));
  CHECK(mailbox.consume(8)); // a stale consumer must not erase the new request
  CHECK(!mailbox.consume(8));
  CHECK(mailbox.request());CHECK(!mailbox.consume(9));CHECK(mailbox.consume(8));
  CHECK(mailbox.request());CHECK(!mailbox.consume(0));CHECK(mailbox.consume(8));
  CHECK(mailbox.request());CHECK(mailbox.consume(8));
  std::thread producer([&]{for(unsigned i=0;i<10000;++i)mailbox.request();});
  for(unsigned i=0;i<10000;++i)mailbox.consume(8);
  producer.join();CHECK(mailbox.request());CHECK(mailbox.consume(8));
  std::thread oldConsumer([&]{for(unsigned i=0;i<10000;++i)mailbox.consume(7);});
  for(unsigned i=0;i<10000;++i)mailbox.request();
  oldConsumer.join();CHECK(mailbox.consume(8));CHECK(!mailbox.consume(8));
  mailbox.disarm();mailbox.arm(9);CHECK(mailbox.request());
  CHECK(!mailbox.consume(8));CHECK(mailbox.consume(9));
  evidence::Ring ring;CHECK(ring.arm(1,256));
  CHECK(!ring.visitFrozen([](const auto&){return true;}));
  for(unsigned i=0;i<1000;++i){evidence::Event e;e.data[0]=i;CHECK(ring.append(1,e));}
  CHECK(ring.finish(1));auto copy=ring.snapshot(true);size_t index=0;
  CHECK(ring.visitFrozen([&](const auto& e){CHECK(index<copy.events.size());
    CHECK(e.sequence==copy.events[index].sequence);CHECK(e.data==copy.events[index].data);++index;return true;}));
  CHECK(index==copy.events.size());index=0;
  CHECK(!ring.visitFrozen([&](const auto&){return ++index<3;}));CHECK(index==3);
  CHECK(ring.visitFrozen([](const auto&){return true;}));CHECK(ring.discard(1));
  CHECK(!ring.visitFrozen([](const auto&){return true;}));
  using Lease=evidence::RecorderControlLease;
  using Authority=evidence::RecorderControlAuthority;
  static_assert(!std::is_copy_constructible_v<Lease> && !std::is_move_constructible_v<Lease>);
  static_assert(!std::is_copy_assignable_v<Lease> && !std::is_move_assignable_v<Lease>);
  static_assert(!std::is_copy_constructible_v<Authority> && !std::is_move_constructible_v<Authority>);
  static_assert(!std::is_copy_assignable_v<Authority> && !std::is_move_assignable_v<Authority>);
  Authority authority(2),other,disabled(0);Lease first,second,foreign,empty,exhausted;
  CHECK(!disabled.claim(empty,true));CHECK(!authority.occupied());
  CHECK(authority.allows(nullptr,false));CHECK(authority.allows(nullptr,true));
  CHECK(!authority.allows(&empty,true));CHECK(!authority.release(empty));
  CHECK(!authority.claim(first,false));CHECK(authority.claim(first,true));
  CHECK(authority.occupied());CHECK(authority.owns(first));
  CHECK(authority.allows(&first,false));CHECK(!authority.allows(nullptr,false));
  CHECK(authority.allows(nullptr,true));CHECK(!authority.claim(second,true));
  CHECK(other.claim(foreign,true));CHECK(!authority.owns(foreign));
  CHECK(!authority.release(foreign));CHECK(!other.release(first));
  CHECK(!authority.allows(&foreign,true));CHECK(authority.owns(first));
  CHECK(authority.release(first));CHECK(!authority.release(first));
  CHECK(!authority.claim(first,true));CHECK(authority.claim(second,true));
  CHECK(!authority.release(first));CHECK(authority.owns(second));
  CHECK(!authority.allows(&first,true));CHECK(authority.release(second));
  CHECK(!authority.claim(exhausted,true));CHECK(!authority.occupied());
  CHECK(authority.allows(nullptr,false));CHECK(other.release(foreign));
  Authority repeated;
  for(unsigned i=0;i<1000;++i){Lease current;
    CHECK(!repeated.claim(current,false));CHECK(repeated.claim(current,true));
    CHECK(repeated.owns(current));CHECK(!repeated.release(first));
    CHECK(repeated.owns(current));CHECK(repeated.release(current));
    CHECK(!repeated.claim(current,true));CHECK(!repeated.occupied());}
  std::cout<<"recorder session/mailbox/production ring checks="<<checks<<" PASS\n";
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
