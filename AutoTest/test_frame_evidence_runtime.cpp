#include "../src/d3d9/war3/tools/war3_frame_evidence.h"
#include "../src/d3d9/war3/tools/war3_frame_evidence_control.h"
#include <iostream>
#include <memory>
#include <thread>
#include <vector>
#include <fstream>
#include <filesystem>

using namespace dxvk::war3::tools::evidence;
using json=nlohmann::json;
static unsigned checks=0;
#define CHECK(x) do{++checks;if(!(x))throw std::runtime_error(#x);}while(false)
int main(int argc,char**) {
  try {
    if(argc>1) {
      CHECK(!Enabled());CHECK(!ActiveSession());
      CHECK(!Control({{"action","arm"}})["ok"].get<bool>());
      std::cout<<"disabled checks="<<checks<<" PASS\n";return 0;
    }
    CHECK(Enabled());CHECK(!ActiveSession());
    CHECK(!Control({{"action","export"}})["ok"].get<bool>());
    CHECK(!Control({{"action","arm"},{"capacity",true}})["ok"].get<bool>());
    CHECK(!Control({{"action","arm"},{"path","outside"}})["ok"].get<bool>());
    auto arm=Control({{"action","arm"},{"capacity",256}});
    CHECK(arm["ok"].get<bool>()); const auto token=arm["session"].get<std::string>();
    const auto generation=ActiveSession();CHECK(generation);
    RecorderControlLease ringBusy;
    CHECK(!ClaimLocalRecorder(ringBusy));CHECK(!OwnsLocalRecorder(ringBusy));
    CHECK(!Control({{"action","arm"}})["ok"].get<bool>());
    CHECK(!Control({{"action","freeze"},{"session","18446744073709551616"}})["ok"].get<bool>());
    CHECK(!Control({{"action","trigger"},{"session",token},{"postPresents",-1}})["ok"].get<bool>());
    const Key key{101,17,3,5};
    {
      Scope pipeline(Kind::PipelineBegin,key,"SyntheticPipeline");
      {Scope pass(Kind::PassBegin,key,"SyntheticShadow",pipeline.id());pass.outcome(1);}
      pipeline.outcome(1);
    }
    CHECK(Control({{"action","trigger"},{"session",token},{"postPresents",1}})["ok"].get<bool>());
    {Scope present(Kind::PresentBegin,key,"SyntheticPresent");present.value(2,1);present.outcome(1);}
    CHECK(!ActiveSession());
    auto exported=Control({{"action","export"},{"session",token}});
    CHECK(exported["ok"].get<bool>());CHECK(!exported["captureComplete"].get<bool>());
    std::cout<<"EXPORT="<<exported["path"].get<std::string>()<<"\n";
    CHECK(!Control({{"action","export"},{"session",token}})["ok"].get<bool>());
    CHECK(Control({{"action","discard"},{"session",token}})["ok"].get<bool>());
    auto next=Control({{"action","arm"},{"capacity",256}});
    CHECK(next["ok"].get<bool>());CHECK(!Record(generation,Event{}));
    const auto nextToken=next["session"].get<std::string>();const auto nextGeneration=ActiveSession();
    std::vector<std::thread> threads;
    for(unsigned t=0;t<4;++t) threads.emplace_back([=]{
      for(unsigned i=0;i<2500;++i) {Event e{};e.key={t,i,3,5};Record(nextGeneration,e);}
    });
    for(auto& t:threads)t.join();
    CHECK(Control({{"action","freeze"},{"session",nextToken}})["ok"].get<bool>());
    auto state=Control({{"action","status"}});
    auto accepted=std::stoull(state["accepted"].get<std::string>());
    auto lost=std::stoull(state["producerLosses"].get<std::string>());
    CHECK(accepted+lost==10000);
    CHECK(Control({{"action","discard"},{"session",nextToken}})["ok"].get<bool>());
    auto third=Control({{"action","arm"},{"capacity",256}});
    auto thirdToken=third["session"].get<std::string>();
    auto stale=std::make_unique<Scope>(Kind::PipelineBegin,key,"OldInFlightScope");
    CHECK(stale->id());
    CHECK(Control({{"action","freeze"},{"session",thirdToken}})["ok"].get<bool>());
    CHECK(Control({{"action","discard"},{"session",thirdToken}})["ok"].get<bool>());
    auto fourth=Control({{"action","arm"},{"capacity",256}});
    stale.reset(); // old scope must not append an end to the new session
    CHECK(Control({{"action","status"}})["accepted"]=="0");
    auto fourthToken=fourth["session"].get<std::string>();
    CHECK(Control({{"action","freeze"},{"session",fourthToken}})["ok"].get<bool>());
    CHECK(Control({{"action","discard"},{"session",fourthToken}})["ok"].get<bool>());
    RecorderControlLease lease,empty;
    RecorderControlAuthority foreignAuthority;RecorderControlLease foreign;
    CHECK(foreignAuthority.claim(foreign,true));
    CHECK(ClaimLocalRecorder(lease));CHECK(!ClaimLocalRecorder(empty));
    CHECK(OwnsLocalRecorder(lease));CHECK(!OwnsLocalRecorder(foreign));
    CHECK(!ReleaseLocalRecorder(foreign));CHECK(!ReleaseLocalRecorder(empty));
    CHECK(foreignAuthority.owns(foreign));CHECK(OwnsLocalRecorder(lease));
    CHECK(Control({{"action","status"}})["ok"].get<bool>());
    CHECK(!Control({{"action","status"}},&empty)["ok"].get<bool>());
    CHECK(!Control({{"action","arm"}},&foreign)["ok"].get<bool>());
    CHECK(!Control({{"action","arm"}})["ok"].get<bool>());
    auto local=Control({{"action","arm"},{"capacity",256}},&lease);
    CHECK(local["ok"].get<bool>());const auto localToken=local["session"].get<std::string>();
    auto localGeneration=ActiveSession();CHECK(!FrozenForExport(localGeneration));
    for(unsigned i=0;i<250;++i){Event e;e.key=key;e.data[0]=i;CHECK(Record(localGeneration,e));}
    CHECK(!Control({{"action","freeze"},{"session",localToken}})["ok"].get<bool>());
    CHECK(Control({{"action","freeze"},{"session",localToken}},&lease)["ok"].get<bool>());
    CHECK(FrozenForExport(localGeneration));CHECK(!FrozenForExport(generation));
    std::atomic<bool> stopping{true};
    CHECK(!Control({{"action","export"},{"session",localToken}},&lease,&stopping)["ok"].get<bool>());
    stopping=false;auto files=Control({{"action","export"},{"session",localToken}},&lease,&stopping);
    CHECK(files["ok"].get<bool>());
    std::ifstream file(std::filesystem::u8path(files["path"].get<std::string>()));json saved;file>>saved;
    CHECK(saved["schema"]==7);CHECK(saved["session"]==localToken);CHECK(saved["effectiveConfiguration"]["localRecorderOwner"]==true);
    CHECK(saved["events"].size()==192);
    for(unsigned i=0;i<192;++i){CHECK(saved["events"][i]["sequence"]==std::to_string(i+59));
      CHECK(saved["events"][i]["data"][0]==std::to_string(i+58));}
    CHECK(!Control({{"action","discard"},{"session",localToken}})["ok"].get<bool>());
    CHECK(Control({{"action","discard"},{"session",localToken}},&lease)["ok"].get<bool>());
    CHECK(ReleaseLocalRecorder(lease));CHECK(!OwnsLocalRecorder(lease));
    CHECK(!Control({{"action","arm"}},&lease)["ok"].get<bool>());
    CHECK(!ClaimLocalRecorder(lease)); // released credential cannot be reissued
    CHECK(ClaimLocalRecorder(ringBusy)); // failed claim did not consume the object
    CHECK(!ReleaseLocalRecorder(lease));CHECK(!ReleaseLocalRecorder(foreign));
    CHECK(!ReleaseLocalRecorder(empty));CHECK(OwnsLocalRecorder(ringBusy));
    CHECK(!Control({{"action","status"}},&lease)["ok"].get<bool>());
    auto resumed=Control({{"action","arm"},{"capacity",256}},&ringBusy);
    CHECK(resumed["ok"].get<bool>());const auto resumedToken=resumed["session"].get<std::string>();
    const auto resumedGeneration=ActiveSession();CHECK(resumedGeneration>localGeneration);
    CHECK(Record(resumedGeneration,Event{}));
    const auto before=Control({{"action","status"}});
    for(const auto* bad:{&lease,&empty,&foreign}){
      CHECK(!Control({{"action","freeze"},{"session",resumedToken}},bad)["ok"].get<bool>());
      CHECK(!Control({{"action","discard"},{"session",resumedToken}},bad)["ok"].get<bool>());
    }
    CHECK(Control({{"action","status"}})==before);CHECK(ActiveSession()==resumedGeneration);
    CHECK(Control({{"action","freeze"},{"session",resumedToken}},&ringBusy)["ok"].get<bool>());
    CHECK(Control({{"action","discard"},{"session",resumedToken}},&ringBusy)["ok"].get<bool>());
    CHECK(ReleaseLocalRecorder(ringBusy));CHECK(!ReleaseLocalRecorder(ringBusy));
    CHECK(Control({{"action","status"}})["effectiveConfiguration"]["localRecorderOwner"]==false);
    CHECK(foreignAuthority.release(foreign));
    // Contend through the actual production controlMutex; no model substitute.
    std::atomic<unsigned> owners{0},successes{0},errors{0};threads.clear();
    for(unsigned t=0;t<4;++t)threads.emplace_back([&]{
      for(unsigned i=0;i<500;++i){RecorderControlLease contender,unissued;
        if(!ClaimLocalRecorder(contender))continue;
        if(owners.fetch_add(1)!=0)++errors;
        if(ReleaseLocalRecorder(unissued)||!OwnsLocalRecorder(contender))++errors;
        if(!Control({{"action","status"}},&contender)["ok"].get<bool>())++errors;
        if(owners.fetch_sub(1)!=1)++errors;
        if(!ReleaseLocalRecorder(contender)||ReleaseLocalRecorder(contender))++errors;
        ++successes;
      }
    });
    for(auto& t:threads)t.join();
    CHECK(successes>0);CHECK(errors==0);CHECK(owners==0);
    CHECK(Control({{"action","status"}})["effectiveConfiguration"]["localRecorderOwner"]==false);
    RecorderControlLease afterContention;CHECK(ClaimLocalRecorder(afterContention));
    CHECK(ReleaseLocalRecorder(afterContention));
    std::cout<<"control contention attempts=2000 successes="<<successes<<" errors="<<errors<<"\n";
    std::cout<<"runtime checks="<<checks<<" accepted="<<accepted<<" explicitLosses="<<lost<<" PASS\n";
  }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
}
