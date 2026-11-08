#define TEST_REAL_AA
#include "startup_control_integration_test.cpp"
using Phase=FlowAnchoredStartup::Phase;
static void fresh(Optimizer &o,float hz=26){
  auto &s=o.instance.status;s.FlowRate=23;s.CompressorFrequency=static_cast<uint8_t>(hz);
  o.wp_return_received_ms_=o.compressor_observed_ms_=test_now;
  float values[]={hz,23,s.HpFeedTemperature,s.HpReturnTemperature,s.Z2FeedTemperature,s.Z2ReturnTemperature};
  for(unsigned i=0;i<GuardTelemetry::COUNT;++i)o.guard_telemetry_.observe(static_cast<GuardTelemetry::Field>(i),test_now,values[i]);
}
static void prepare(Optimizer &o){
  o.flow_enabled.state=o.buffer_enabled.state=true;o.initial_delta.state=1;o.flow_interval.state=15;o.duration.state=60;
  o.room=o.room_target=19;o.zone_min=o.min_z2.state=22;o.zone_max=o.max_z2.state=31;
  o.compressor_observer_=CompressorStateTracker{};o.confirmed_compressor_run_=false;
  fresh(o);o.register_compressor_observer_();o.instance.receive_compressor(false);fresh(o);
}
static void fire(Optimizer &o){o.instance.receive_compressor(true);}
static void tick(Optimizer &o,uint32_t ms=30000,float hz=26){test_now+=ms;fresh(o,hz);o.service_z2_flow_ramp();}
static void handover(Optimizer &o){
  prepare(o);o.instance.status.MixingTankTemperature=26;o.feed=o.instance.status.Z2FeedTemperature=26;fresh(o);fire(o);
  for(int i=0;i<10;++i)tick(o);
  check(o.z2_flow_ramp_.phase()==Phase::HANDOVER,"fixture enters real buffered handover");
}
int main(){
  {
    Optimizer o;prepare(o);auto &s=o.instance.status;s.MixingTankTemperature=26;fire(o);
    check(near(o.live,21.5f)&&o.z2_flow_ramp_.phase()==Phase::BUFFER_WARMUP,"real start bypasses configured min 22 with fixed 21.5 request");
    for(int i=0;i<60;++i)tick(o);
    check(o.z2_flow_ramp_.phase()==Phase::BUFFER_WARMUP,"warm THW10 with cold Z2 does not trigger readiness");
    check(near(o.z2_flow_ramp_.buffer_threshold(),26),"actual AA 26.5 and margin0.5 produce threshold26");
    o.feed=s.Z2FeedTemperature=26;const auto entered=test_now;
    tick(o);
    for(int i=0;i<9;++i)tick(o);
    check(o.z2_flow_ramp_.phase()==Phase::BUFFER_WARMUP&&near(o.live,25),"guard protects warmup while readiness waits full five minutes");
    tick(o);check(o.z2_flow_ramp_.phase()==Phase::HANDOVER&&near(o.live,25),"buffer readiness hands over from actual guard-raised request25");
    for(int i=0;i<5;++i)tick(o);
    check(near(o.live,25),"handover waits three minutes before raising");
    tick(o);check(near(o.live,25.5f),"existing writer raises only 0.5 after three minutes");
    tick(o);check(near(o.live,25.5f),"30 second AA update cannot accelerate handover");
    for(int i=0;i<11;++i)tick(o);
    check(!o.z2_flow_ramp_.active()&&near(o.live,26.5f),"handover finishes at normal AA target26.5");
    check(o.processed[0]==1&&o.override_z1.changes==0,"Z2-only services leave Z1 and overrides unchanged");
    (void)entered;
  }
  for(int kind=0;kind<5;++kind){
    Optimizer o;prepare(o);auto &s=o.instance.status;s.MixingTankTemperature=26;
    if(kind==0)s.DipSwitch2=(1<<6); // No mixing tank configured, still independent flag; has_2zones becomes false.
    if(kind==1)s.MixingTankTemperature=NAN;
    if(kind==2)s.MixingTankTemperature=81;
    if(kind==3)s.MixingTankTemperature=-1;
    if(kind==4)o.buffer_enabled.state=false;
    if(kind==0){check(!o.flow_ramp_buffer_().valid,"Mitsubishi configuration controls THW10 validity");continue;}
    fire(o);for(int i=0;i<31;++i)tick(o,60000);
    check(o.z2_flow_ramp_.phase()==Phase::BUFFER_WARMUP,"invalid/off buffer mode keeps time-only startup");
    check(near(o.z2_flow_ramp_.cap(),22.5f),"fallback follows fixed base21.5 plus two quarter-hour steps");
  }
  {
    Optimizer o;prepare(o);o.instance.status.MixingTankTemperature=26;fire(o);
    for(int i=0;i<31;++i)tick(o,60000,42);
    check(o.flow_ramp_buffer_().valid&&o.z2_flow_ramp_.active(),"unchanged THW10 for 31 minutes remains valid at42Hz");
    o.guard_telemetry_.received[GuardTelemetry::HP_FEED]=test_now-120001;o.service_z2_flow_ramp();
    check(!o.z2_flow_ramp_.active(),"water telemetry communications timeout still cancels special control");
    Optimizer freq;prepare(freq);freq.instance.status.MixingTankTemperature=NAN;fire(freq);
    for(float hz:{16.0f,26.0f,36.0f,42.0f}){tick(freq,30000,hz);check(freq.z2_flow_ramp_.active(),"frequency never holds or releases startup");}
    freq.guard_telemetry_.observed[GuardTelemetry::FREQUENCY]=false;freq.service_z2_flow_ramp();
    check(freq.z2_flow_ramp_.active(),"missing diagnostic frequency is not a ramp eligibility condition");
  }
  for(float minimum:{20.0f,22.0f,24.0f}){
    Optimizer o;prepare(o);o.zone_min=o.min_z2.state=minimum;o.buffer_enabled.state=false;fire(o);
    check(near(o.live,21.5f),"configured minimum is not added to startup base");
    const auto p=o.get_heating_profile_(0);o.flow_enabled.state=false;o.service_z2_flow_ramp(true);
    float normal=o.calculate_heating_flow_(1,o.instance.status,p,true,7,minimum,31,2,0);
    check(near(normal,minimum==24?24:22.5f),"real AA/guard writer honors configured min20/22/24 after startup");
    check(near(o.min_z1.state,24)&&near(o.max_z1.state,35),"Z1 bounds unchanged");
  }
  {
    Optimizer o;prepare(o);o.instance.status.MixingTankTemperature=NAN;fire(o);
    o.room=18.8f;tick(o);for(int i=0;i<19;++i)tick(o);
    check(o.z2_flow_ramp_.active(),"0.2 K deficit remains allowed before ten minutes");
    tick(o);check(!o.z2_flow_ramp_.active(),"0.2 K deficit ten minutes releases through real service");
    Optimizer urgent;prepare(urgent);fire(urgent);urgent.room=18.5f;tick(urgent,5000);
    check(!urgent.z2_flow_ramp_.active(),"0.5 K deficit immediately releases through service");
  }
  {
    Optimizer o;prepare(o);o.instance.status.MixingTankTemperature=NAN;fire(o);
    o.feed=o.instance.status.Z2FeedTemperature=26;
    tick(o);tick(o);o.feed=o.instance.status.Z2FeedTemperature=21;tick(o);
    check(o.z2_flow_ramp_.active(),"short guard episodes do not cancel");
    o.feed=o.instance.status.Z2FeedTemperature=26;tick(o);
    for(int i=0;i<9;++i)tick(o);
    check(o.z2_flow_ramp_.active(),"sustained guard still waits full five minutes");
    tick(o);check(!o.z2_flow_ramp_.active()&&near(o.live,26.5f),"five minute sustained guard releases normal AA");
  }
  for(int reason=0;reason<8;++reason){
    Optimizer o;handover(o);
    if(reason==0)o.instance.status.DefrostActive=true;
    if(reason==1)o.instance.status.Operation=esphome::ecodan::Status::OperationMode::DHW_ON;
    if(reason==2)o.instance.receive_compressor(false);
    if(reason==3)o.room=18.5f;
    if(reason==4)o.observe_compressor_state_(NAN);
    if(reason==5)o.flow_enabled.state=false;
    if(reason==6)o.instance.status.MultiZoneStatus=0;
    if(reason==7)o.odin=true;
    o.service_z2_flow_ramp();check(!o.z2_flow_ramp_.active(),"handover also terminates for safety/demand/ownership changes");
  }
  {
    Optimizer o;handover(o);o.reject_z2_writes=true;
    for(int i=0;i<6;++i)tick(o);
    check(near(o.live,25)&&near(o.z2_flow_ramp_.last_requested(),25),"rejected write cannot advance actual request baseline");
    tick(o);check(near(o.z2_flow_ramp_.cap(),25.5f),"retries do not compound unqueued handover steps");
    o.reject_z2_writes=false;tick(o);check(near(o.live,25.5f),"first accepted retry advances only one 0.5 K step");
    tick(o);check(near(o.live,25.5f),"accepted retry starts a new three minute interval");
  }
  std::printf("PASS: %d production THW10/service/handover/communications/minimum checks.\n",checks);
}
