#include "components/optimizer/flow_anchored_startup.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
using esphome::optimizer::FlowAnchoredStartup;
static int checks=0;
static void check(bool ok,const char *s){++checks;if(!ok){std::fprintf(stderr,"FAIL flow ramp: %s\n",s);std::exit(1);}}
static bool near(float a,float b){return std::fabs(a-b)<0.001f;}
static FlowAnchoredStartup::Result calc(FlowAnchoredStartup &r,uint32_t t,float normal=30,float room=19,float target=19){
  return r.calculate(t,true,true,normal,room,target,0.5f,5,45,24,31);
}
static void start(FlowAnchoredStartup &r,uint32_t t=0){check(r.compressor_event(t,true,false,false,true,true,20.5f,2),"genuine start accepted");}
int main(){
  {
    FlowAnchoredStartup r;
    check(!r.compressor_event(0,true,false,true,true,true,20.5f,2)&&!r.active(),"initial ON at ESP boot never arms");
    check(!r.compressor_event(0,true,true,false,true,true,20.5f,2),"duplicate ON never arms");
    check(!r.compressor_event(0,true,false,false,false,true,20.5f,2)&&!r.claimed_cycle(),"default OFF does not claim cycle");
    start(r);check(near(r.initial_return(),20.5f)&&near(r.base(),22.5f),"base captured exactly once");
    check(!r.compressor_event(1000,true,true,false,true,true,27,4)&&near(r.base(),22.5f),"later rising return cannot reanchor");
    for(unsigned m=0;m<=40;m+=5){auto out=calc(r,m*60000);check(out.apply&&near(out.cap,22.5f+m/5*0.5f),"specified five-minute cap table");}
    check(!calc(r,45*60000).apply&&!r.active(),"maximum duration ends at exact boundary");
    check(r.claimed_cycle(),"old ramps stay suppressed after completion");
    r.compressor_event(46*60000,false,true,false,true,true,20,2);check(!r.claimed_cycle(),"stop releases cycle ownership");
  }
  {
    FlowAnchoredStartup r;start(r);
    check(near(calc(r,299999).cap,22.5f)&&near(calc(r,300000).cap,23),"interval boundary uses floor");
    check(near(calc(r,300000,21.8f).target,21.8f),"lower normal AA is always allowed");
    check(near(calc(r,300000,21.8f).minimum,21.8f),"minimum exception tracks startup request");
    check(near(calc(r,300000,10).target,20),"technical floor remains 20 C");
  }
  for(float hz:{26.0f,16.0f}){
    FlowAnchoredStartup r;start(r);
    for(unsigned t=0;t<=300000;t+=30000){r.observe_frequency(t,hz);calc(r,t,30,18.9f);}
    check(r.holding()&&near(r.cap(),23),"five continuous minutes at 26/16 Hz enter HOLD");
    for(unsigned t=330000;t<=600000;t+=30000){r.observe_frequency(t,hz);calc(r,t,30,18.9f);}
    check(r.holding()&&near(r.cap(),23),"HOLD freezes cap despite elapsed time and rising normal AA");
    check(near(calc(r,600000,22).target,22),"HOLD still permits normal AA reductions");
    r.observe_frequency(601000,31);auto out=calc(r,601000);
    check(!r.holding()&&near(out.cap,23.5f),"frequency over 30 releases HOLD to elapsed-time cap");
  }
  {
    FlowAnchoredStartup r;start(r);
    for(unsigned t=0;t<=300000;t+=30000){r.observe_frequency(t,26);calc(r,t);}
    calc(r,330000,30,18.85f);
    check(!r.holding()&&r.active(),"room deficit 0.15 K releases HOLD while ramp remains");
    check(!calc(r,360000,30,18.8f).apply&&!r.active(),"exact 0.2 K room deficit releases ramp");
    check(!calc(r,370000).apply,"room recovery does not restart mid-cycle");
  }
  {
    FlowAnchoredStartup r;start(r);
    for(unsigned t=0;t<=300000;t+=30000){r.observe_frequency(t,26);calc(r,t);}
    r.observe_frequency(301000,9);check(!r.holding(),"frequency below 10 releases HOLD");
    r.observe_frequency(302000,26);r.observe_frequency(422001,26);
    check(calc(r,422001).apply&&!r.holding(),"frequency gap resets continuous qualification");
    r.observe_frequency(450000,NAN);check(!r.holding(),"invalid frequency cannot retain HOLD");
  }
  {
    FlowAnchoredStartup r;start(r);
    for(unsigned t=0;t<300000;t+=30000){r.observe_frequency(t,26);calc(r,t);}
    r.observe_frequency(299999,40);r.observe_frequency(300000,26);calc(r,300000);
    check(!r.holding(),"brief frequency excursion between evaluations resets five minute qualification");
    for(unsigned t=330000;t<45*60000;t+=30000){r.observe_frequency(t,26);calc(r,t);}
    check(r.holding(),"HOLD can qualify again after excursion");
    r.observe_frequency(45*60000,26);
    check(!calc(r,45*60000).apply&&!r.holding(),"HOLD never extends maximum startup duration");
  }
  {
    FlowAnchoredStartup r;start(r);
    check(!r.guard_result(0,true),"first stop guard wins without immediate cancellation");
    for(int i=0;i<10;++i)check(!r.guard_result(1000,true),"duplicate callbacks do not count as repeated interventions");
    check(r.guard_result(30000,true)&&!r.active(),"second distinct stop guard releases ramp");
    FlowAnchoredStartup s;start(s);s.guard_result(0,true);s.guard_result(30000,false);
    check(!s.guard_result(60000,true)&&s.active(),"uninterrupted guard-free evaluation resets repetition count");
  }
  {
    FlowAnchoredStartup r;start(r);
    check(!r.calculate(0,false,true,30,19,19,.5f,5,45,24,31).apply&&!r.active(),"manual OFF ends ramp");
    start(r);check(!r.calculate(0,true,false,30,19,19,.5f,5,45,24,31).apply,"defrost/DHW/ineligible ends ramp");
    start(r);check(!calc(r,1000,NAN).apply,"invalid normal AA ends ramp");
    start(r);check(!calc(r,1000,30,NAN).apply,"invalid room reading ends ramp");
    FlowAnchoredStartup bad;check(!bad.compressor_event(0,true,false,false,true,true,NAN,2),"invalid start return rejected");
    check(!bad.compressor_event(0,true,false,false,true,true,20,NAN),"invalid delta rejected");
    FlowAnchoredStartup hot;hot.compressor_event(0,true,false,false,true,true,50,5);
    check(near(calc(hot,0,60).target,31),"configured maximum wins over high base");
    FlowAnchoredStartup wrap;start(wrap,UINT32_MAX-10000);
    check(near(calc(wrap,289999).cap,23),"millis wrap preserves interval arithmetic");
    check(!calc(wrap,2690000).apply,"millis wrap preserves timeout");
  }
  std::printf("PASS: %d flow-anchored ramp state/clock/HOLD checks.\n",checks);
}
