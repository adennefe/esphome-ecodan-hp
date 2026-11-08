// Regression scenarios retained; superseded frequency HOLD and two-hit abort tests now assert the new policy.
#define EPS 192  // Xtensa ESP32-S3 specreg.h uses this macro before optimizer headers.
#include "components/optimizer/flow_anchored_startup.h"
#include <cstdio>
#include <cstdlib>
using esphome::optimizer::FlowAnchoredStartup;
static int checks=0;
static void check(bool ok,const char *s){++checks;if(!ok){std::fprintf(stderr,"FAIL flow ramp: %s\n",s);std::exit(1);}}
static bool near(float a,float b){return std::fabs(a-b)<.001f;}
static FlowAnchoredStartup::Result calc(FlowAnchoredStartup &r,uint32_t t,float normal=30,float room=19,float target=19){
  auto out=r.calculate(t,true,true,normal,room,target,.5f,5,45,24,31);
  if(out.apply)r.record_request(t,out.target);return out;
}
static void start(FlowAnchoredStartup &r,uint32_t t=0){r.compressor_event(t,false,true,false,true,true,20.5f,2);check(r.compressor_event(t,true,false,false,true,true,20.5f,2),"genuine start accepted");}
int main(){
  check(EPS==192,"optimizer header compiles with and preserves the SDK register macro");
  {
    FlowAnchoredStartup r;check(r.phase()==FlowAnchoredStartup::Phase::WAITING,"initial WAITING");
    check(!r.compressor_event(0,true,false,true,true,true,20.5f,2)&&!r.active(),"initial ON at ESP boot never arms");
    check(r.phase()==FlowAnchoredStartup::Phase::NORMAL_AA,"boot during ON uses NORMAL_AA");
    check(!r.compressor_event(0,true,true,false,true,true,20.5f,2),"duplicate ON never arms");
    check(!r.compressor_event(0,true,false,false,false,true,20.5f,2)&&!r.claimed_cycle(),"OFF does not claim cycle");
    start(r);check(near(r.initial_return(),20.5f)&&near(r.base(),22.5f),"fixed anchor captured once");
    check(!r.compressor_event(1000,true,true,false,true,true,27,4)&&near(r.base(),22.5f),"rising return cannot reanchor");
    for(unsigned m=0;m<=40;m+=5){auto out=calc(r,m*60000);check(out.apply&&near(out.cap,22.5f+m/5*.5f),"time cap table independent of frequency");}
    check(!calc(r,45*60000).apply&&!r.active(),"maximum duration ends exactly");
    check(r.claimed_cycle(),"legacy ramps remain suppressed for completed cycle");
    r.compressor_event(46*60000,false,true,false,true,true,20,2);
    check(!r.claimed_cycle()&&r.phase()==FlowAnchoredStartup::Phase::WAITING,"stop resets to WAITING");
  }
  {
    FlowAnchoredStartup r;start(r);
    check(near(calc(r,299999).cap,22.5f)&&near(calc(r,300000).cap,23),"floor at exact interval");
    check(near(calc(r,300000,21.8f).target,21.8f),"lower AA allowed");
    check(near(calc(r,300000,21.8f).minimum,21.8f),"startup minimum exception");
    check(near(calc(r,300000,10).target,20),"technical floor 20");
    check(near(calc(r,600000).cap,23.5f)&&r.active(),"no frequency HOLD after five minutes");
  }
  {
    FlowAnchoredStartup r;start(r);
    calc(r,0,30,18.9f);check(r.active(),"room deficit 0.1 K allowed");
    calc(r,300000,30,18.85f);check(r.active(),"room deficit 0.15 K allowed");
    calc(r,360000,30,18.8f);check(r.active(),"room deficit 0.2 K not immediate abort");
    calc(r,959999,30,18.8f);check(r.active(),"ten minute room duration not yet met");
    check(!calc(r,960000,30,18.8f).apply&&!r.active(),"ten minute continuous room deficit releases");
    check(!calc(r,970000).apply,"room recovery cannot restart cycle");
    start(r);calc(r,0,30,18.8f);calc(r,300000,30,18.9f);calc(r,600000,30,18.8f);
    check(r.active(),"room recovery resets sustained-deficit timer");
    calc(r,600001,30,18.5f);check(!r.active(),"room deficit 0.5 K releases immediately");
  }
  {
    FlowAnchoredStartup r;start(r);
    check(!r.guard_result(0,true,22,23),"first guard wins without abort");
    for(int i=0;i<10;++i)check(!r.guard_result(1000,true,22,23),"duplicate evaluations cannot accelerate timer");
    check(!r.guard_result(30000,true,22,23)&&r.active(),"second guard does not end startup");
    check(!r.guard_result(299999,true,22,23),"five minute guard duration not yet met");
    check(r.guard_result(300000,true,22,23)&&!r.active(),"five minute guard override releases");
    FlowAnchoredStartup s;start(s);s.guard_result(0,true,22,23);s.guard_result(180000,false,22,22);
    check(!s.guard_result(360000,true,22,23)&&s.active(),"guard-free check resets timer");
    check(!s.guard_result(660000,true,22,22.49f)&&s.active(),"uplift below 0.5 K does not qualify");
  }
  {
    FlowAnchoredStartup r;start(r);check(!r.calculate(0,false,true,30,19,19,.5f,5,45,24,31).apply,"manual OFF ends startup");
    start(r);check(!r.calculate(0,true,false,30,19,19,.5f,5,45,24,31).apply,"ineligible ends startup");
    start(r);check(!calc(r,1000,NAN).apply,"invalid AA ends startup");
    start(r);check(!calc(r,1000,30,NAN).apply,"invalid room ends startup");
    FlowAnchoredStartup bad;check(!bad.compressor_event(0,true,false,false,true,true,NAN,2),"invalid start return rejected");
    check(!bad.compressor_event(0,true,false,false,true,true,20,NAN),"invalid delta rejected");
    FlowAnchoredStartup hot;hot.compressor_event(0,true,false,false,true,true,50,5);
    check(near(calc(hot,0,60).target,31),"max overrides high base");
    FlowAnchoredStartup wrap;start(wrap,UINT32_MAX-10000);
    check(near(calc(wrap,289999).cap,23),"millis wrap intervals");
    check(!calc(wrap,2690000).apply,"millis wrap timeout");
  }
  std::printf("PASS: %d flow-anchored lifecycle/room/guard/time-only checks.\n",checks);
}
