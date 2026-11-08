#include "components/optimizer/flow_anchored_startup.h"
#include <cstdio>
#include <cstdlib>
using esphome::optimizer::FlowAnchoredStartup;
using Phase=FlowAnchoredStartup::Phase;
using Buffer=FlowAnchoredStartup::Buffer;
static int checks=0;
static void check(bool ok,const char *s){++checks;if(!ok){std::fprintf(stderr,"FAIL buffer startup: %s\n",s);std::exit(1);}}
static bool near(float a,float b){return std::fabs(a-b)<.001f;}
static void start(FlowAnchoredStartup &r){check(r.compressor_event(0,true,false,false,true,true,20.5f,1),"arm fixed 21.5 reference");}
static Buffer ready(){return {true,true,25.5f,25.5f,.5f,5};}
static auto calc(FlowAnchoredStartup &r,uint32_t t,Buffer b,float normal=26,float minimum=22,float room=19){
  auto out=r.calculate(t,true,true,normal,room,19,.5f,15,60,minimum,31,b);
  if(out.apply)r.record_request(t,out.target);return out;
}
int main(){
  {
    FlowAnchoredStartup waiting;waiting.cancel("operation OFF during initialization");
    check(waiting.phase()==Phase::WAITING,"off-mode notifications cannot consume next genuine start");
    check(waiting.compressor_event(0,true,false,false,true,true,20.5f,1),"first genuine start after initialization cancellation still arms");
    waiting.cancel("completed");
    check(!waiting.compressor_event(1000,true,false,false,true,true,24,1),"even a duplicate edge cannot restart claimed cycle");
  }
  check(FlowAnchoredStartup::valid_thw10(true,0)&&FlowAnchoredStartup::valid_thw10(true,80),"THW10 inclusive plausible bounds");
  for(float value:{NAN,INFINITY,-1.0f,81.0f})check(!FlowAnchoredStartup::valid_thw10(true,value),"invalid THW10 rejected");
  check(!FlowAnchoredStartup::valid_thw10(false,25.5f),"sensor absent in Mitsubishi configuration");
  for(int invalid=0;invalid<6;++invalid){
    FlowAnchoredStartup r;start(r);auto b=ready();
    if(invalid==0)b.enabled=false;
    if(invalid==1)b.valid=false;
    if(invalid==2)b.temperature=NAN;
    if(invalid==3)b.temperature=81;
    if(invalid==4)b.temperature=-1;
    if(invalid==5)b.temperature=INFINITY;
    for(unsigned minute=0;minute<=45;minute+=15){auto out=calc(r,minute*60000,b);check(out.apply&&near(out.cap,21.5f+minute/15*.5f),"unavailable/disabled THW10 retains specified time ramp");}
    check(r.phase()==Phase::BUFFER_WARMUP,"fallback does not invent readiness");
  }
  {
    FlowAnchoredStartup r;start(r);auto b=ready();
    check(near(calc(r,0,b).target,21.5f),"configured min 22 is not a second start reference");
    check(near(r.buffer_threshold(),25.5f),"max(min22, normal26-margin0.5)=25.5");
    calc(r,299999,b);check(r.phase()==Phase::BUFFER_WARMUP,"readiness not before five minutes");
    auto out=calc(r,300000,b);check(r.phase()==Phase::HANDOVER&&near(out.target,21.5f),"readiness begins handover at last actual request");
    check(r.ready_ms(300000)==300000,"condition duration preserved in diagnostics");
    check(near(calc(r,479999,b).target,21.5f),"handover cannot rise before three minutes");
    check(near(calc(r,480000,b).target,22),"handover adds at most 0.5 K after three minutes");
    for(int i=0;i<10;++i)check(near(calc(r,480000,b).target,22),"duplicate calls do not accumulate handover increments");
    check(near(calc(r,1000000,b).target,22.5f),"late evaluation has no catch-up jump");
    uint32_t time=1000000;
    while(r.active()){time+=180000;auto step=calc(r,time,b);check(step.target<=26.001f,"handover stays below normal target");}
    check(r.phase()==Phase::NORMAL_AA&&near(r.last_requested(),25.5f),"normal target ends limiter; writer owns final normal request");
    check(!calc(r,time+1000,b).apply,"same cycle never restarts startup");
  }
  {
    FlowAnchoredStartup r;start(r);auto b=ready();calc(r,0,b);
    r.record_request(0,24.5f);auto out=calc(r,300000,b);
    check(r.phase()==Phase::HANDOVER&&near(out.target,24.5f),"handover uses guarded request, not original 21.5 base");
    check(near(calc(r,480000,b).target,25),"guarded baseline only rises 0.5 K");
    check(near(calc(r,510000,b,24).target,24)&&r.phase()==Phase::NORMAL_AA,"normal AA reduction allowed immediately");
  }
  {
    FlowAnchoredStartup r;start(r);auto b=ready();b.z2_feed=24;
    for(unsigned m=0;m<=30;++m){auto out=calc(r,m*60000,b);check(out.apply&&r.phase()==Phase::BUFFER_WARMUP,"unchanged THW10 stays valid for thirty minutes; cold Z2 blocks readiness");}
    check(FlowAnchoredStartup::valid_thw10(true,b.temperature),"no THW10 ageing or change requirement");
    b.z2_feed=25.5f;calc(r,1800000,b);calc(r,2099999,b);check(r.phase()==Phase::BUFFER_WARMUP,"new supply qualification waits full duration");
    calc(r,2100000,b);check(r.phase()==Phase::HANDOVER,"same unchanged THW10 enables readiness after Z2 catches up");
  }
  {
    FlowAnchoredStartup r;start(r);auto b=ready();calc(r,0,b);b.temperature=25.4f;calc(r,240000,b);
    b.temperature=25.5f;calc(r,270000,b);calc(r,540000,b);
    check(r.phase()==Phase::BUFFER_WARMUP,"short threshold excursion resets readiness timer");
    calc(r,570000,b);check(r.phase()==Phase::HANDOVER,"full new five minute duration qualifies");
    FlowAnchoredStartup s;start(s);b=ready();b.temperature=24;calc(s,0,b);b.temperature=25.5f;calc(s,60000,b);
    calc(s,240000,b,27);check(near(s.buffer_threshold(),26.5f)&&s.ready_ms(240000)==0,"rising AA dynamically raises threshold and resets readiness");
    b.temperature=b.z2_feed=26.5f;calc(s,300000,b,27);calc(s,600000,b,27);
    check(s.phase()==Phase::HANDOVER,"rising THW10 can meet new dynamic threshold");
  }
  for(float minimum:{20.0f,22.0f,24.0f}){
    FlowAnchoredStartup r;start(r);auto b=ready();b.temperature=b.z2_feed=24;
    calc(r,0,b,22.5f,minimum);check(near(r.buffer_threshold(),std::max(minimum,22.0f)),"threshold respects each configured minimum");
    calc(r,300000,b,22.5f,minimum);uint32_t time=300000;
    while(r.active()&&time<3600000){time+=180000;calc(r,time,b,22.5f,minimum);}
    const float normal=std::clamp(22.5f,minimum,31.0f);
    check(!r.active()&&near(normal,minimum==24?24:22.5f),"normal AA minimum 20/22/24 restored after handover");
  }
  {
    FlowAnchoredStartup r;start(r);auto b=ready();b.z2_feed=20;calc(r,0,b);b.z2_feed=25.5f;
    calc(r,3200000,b);calc(r,3500000,b);check(r.phase()==Phase::HANDOVER,"late buffer readiness can start handover");
    check(!calc(r,3600000,b).apply&&r.phase()==Phase::NORMAL_AA,"handover shares total sixty minute deadline");
  }
  std::printf("PASS: %d THW10 validity/condition/handover/minimum checks.\n",checks);
}
