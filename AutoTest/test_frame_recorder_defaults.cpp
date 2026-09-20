#include "../src/d3d9/war3/tools/war3_frame_evidence.h"
#include "../src/d3d9/war3/tools/war3_frame_evidence_control.h"
#include "../src/d3d9/war3/tools/war3_frame_recorder_config.h"
#include "../src/d3d9/war3/tools/war3_frame_recorder_profile.h"
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
using namespace dxvk::war3::tools::evidence;
unsigned checks=0;
#define CHECK(x) do{++checks;if(!(x))throw std::runtime_error(#x);}while(false)
int main(int argc,char** argv){try{
  CHECK(argc==3);const std::string mode=argv[1];const bool internal=std::string(argv[2])=="1";
  const char* keys[]={"DXVK_WAR3_FRAME_EVIDENCE","DXVK_WAR3_FRAME_EVIDENCE_INPUTS",
    "DXVK_WAR3_FRAME_EVIDENCE_DRAWS","DXVK_WAR3_FRAME_HISTORY_SELF_CONTAINED"};
  for(auto key:keys){CHECK(_putenv_s(key,"")==0);CHECK(std::getenv(key)==nullptr);}
  bool enabled=internal,inputs=internal,draws=internal,local=internal;
  if(mode!="default"){
    for(auto key:keys)CHECK(_putenv_s(key,"1")==0);
    enabled=inputs=draws=local=true;
    if(mode=="disabled"||mode=="invalid"){
      CHECK(_putenv_s(keys[0],mode=="disabled"?"0":"true")==0);enabled=inputs=draws=local=false;
    }else if(mode=="external") {CHECK(_putenv_s(keys[3],"0")==0);local=false;}
    else CHECK(mode=="explicit-on");
  }
  // Calls the linked production implementation, not just the policy model.
  CHECK(InternalRecorderBuild()==internal);CHECK(Enabled()==enabled);
  CHECK(InputsEnabled()==inputs);CHECK(DrawsEnabled()==draws);CHECK(LocalRecorderEnabled()==local);
  CHECK(!ActiveSession());
  auto armed=Control({{"action","arm"},{"capacity",256}});
  CHECK(armed["ok"].get<bool>()==enabled);
  if(enabled){const auto token=armed["session"].get<std::string>();CHECK(ActiveSession()!=0);
    CHECK(Control({{"action","freeze"},{"session",token}})["ok"].get<bool>());
    CHECK(Control({{"action","discard"},{"session",token}})["ok"].get<bool>());}
  CHECK(!ActiveSession());
  for(bool profile:{false,true})for(auto value:{static_cast<const char*>(nullptr),"","0","1","true"," 1","-1"}){
    const bool flag=value?std::string(value)=="1":profile;
    auto c=ResolveRecorderConfiguration(profile,value,"1","1","1",nullptr);
    CHECK(c.enabled==flag&&c.inputs==flag&&c.draws==flag&&c.local==flag);
    // 2026-09-17 Step 1③：paletteObject 子门默认关（未设 ⇒ false），且受主门约束。
    CHECK(!c.paletteObject);
    c=ResolveRecorderConfiguration(profile,"1",value,value,value,nullptr);
    CHECK(c.enabled&&c.inputs==flag&&c.draws==flag&&c.local==flag);
    CHECK(!c.paletteObject);
    // 子门显式开：主门开 ⇒ true；主门关 ⇒ 仍为 false（受主门约束）。
    CHECK(ResolveRecorderConfiguration(true,"1","1","1","1","1").paletteObject);
    CHECK(!ResolveRecorderConfiguration(true,"0","1","1","1","1").paletteObject);
    // 主门关（构建默认关且 env 未设）⇒ 子门仍为 false。
    // 原写法为 ResolveRecorderConfiguration(false,"1",...)：master="1" 会把主门打开
    // （enabled=RecorderFlag(master,internal)），与"主门关 ⇒ 子门 false"的断言注释矛盾。
    CHECK(!ResolveRecorderConfiguration(false,nullptr,"1","1","1","1").paletteObject);
    // 显式 0 / 非法值一律 fail-closed。
    CHECK(!ResolveRecorderConfiguration(true,"1","1","1","1","0").paletteObject);
    CHECK(!ResolveRecorderConfiguration(true,"1","1","1","1","true").paletteObject);
  }
  CHECK(!RecorderDiskBudget(false,UINT64_MAX));CHECK(!RecorderDiskBudget(true,RecorderDiskHeadroom-1));
  CHECK(RecorderDiskBudget(true,RecorderDiskHeadroom));CHECK(RecorderDiskBudget(true,UINT64_MAX));
  const auto profile=DefaultRecorderProfile(internal);
  if(internal){
    CHECK(profile.highPressure);CHECK(profile.cpuEvents==65536);CHECK(profile.imagePreFrames==96);
    CHECK(profile.imagePostFrames==4);CHECK(profile.preMilliseconds==1000);CHECK(profile.inputSlots==224);
  }else{
    CHECK(!profile.highPressure);CHECK(profile.cpuEvents==262144);CHECK(profile.imagePreFrames==256);
    CHECK(profile.imagePostFrames==4);CHECK(profile.preMilliseconds==1000);CHECK(profile.inputSlots==576);
  }
  // Exercise the production override, including the actual getenv adapter.
  const char* preKey="DXVK_WAR3_FRAME_EVIDENCE_PRE_FRAMES";
  const char* postKey="DXVK_WAR3_FRAME_EVIDENCE_POST_FRAMES";
  for (bool selectedInternal : {false,true}) {
    const auto selected=DefaultRecorderProfile(selectedInternal);
    CHECK(RecorderReducedFrameLimit(nullptr,4,selected.imagePreFrames)==selected.imagePreFrames);
    for(const char* invalid : {"","0","-1","+4"," 4","4 ","4x","1.5","4294967296",
                              "999999999999999999999999999999999999999999999999"}) {
      CHECK(RecorderReducedFrameLimit(invalid,4,selected.imagePreFrames)==selected.imagePreFrames);
    }
    for(uint32_t n=0;n<=300;++n) {
      const auto value=std::to_string(n);
      CHECK(_putenv_s(preKey,value.c_str())==0);
      CHECK(_putenv_s(postKey,value.c_str())==0);
      const auto actual=WithEnvOverrides(selected);
      CHECK(actual.imagePreFrames==((n>=4&&n<=selected.imagePreFrames)?n:selected.imagePreFrames));
      CHECK(actual.imagePostFrames==((n>=1&&n<=selected.imagePostFrames)?n:selected.imagePostFrames));
      CHECK(actual.cpuEvents==selected.cpuEvents&&actual.inputSlots==selected.inputSlots);
      CHECK(actual.preMilliseconds==selected.preMilliseconds&&actual.highPressure==selected.highPressure);
    }
    CHECK(RecorderReducedFrameLimit("0004",4,selected.imagePreFrames)==4);
  }
  CHECK(_putenv_s(preKey,"")==0);CHECK(_putenv_s(postKey,"")==0);
  std::cout<<"recorder production defaults profile="<<internal<<" mode="<<mode<<" checks="<<checks<<" PASS\n";
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
